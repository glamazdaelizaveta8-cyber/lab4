#define NOMINMAX
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

constexpr int M = 3;                 // кількість полів
constexpr int CHUNKS = 6;            // 6 файлів на сценарій (ділиться на 1, 2 і 3 потоки)
constexpr int CHUNK_OPS = 1'000'000; // операцій в одному файлі (разом 6 000 000)
constexpr int RUNS = 5;              // скільки разів повторюємо вимірювання

// ---------------------------------------------------------------------------
// Структура даних. Варіант 7:
//   поле 0: read 5%,  write 5%
//   поле 1: read 30%, write 5%
//   поле 2: read 24%, write 1%
//   string: 30%
// Схема захисту: по одному shared_mutex на кожне поле (разом 3).
//   read  -> shared_lock  (читачі одного поля не блокують один одного)
//   write -> unique_lock  (лише на одному полі)
//   string -> shared_lock на всі 3 поля в порядку 0,1,2 (немає взаємоблокувань,
//             бо писальники беруть лише один м'ютекс)
// Кожен слот вирівняно на 64 байти, щоб м'ютекси різних полів не лежали
// в одному рядку кешу (false sharing).
// ---------------------------------------------------------------------------
class SharedData {
    struct alignas(64) Slot {
        int value = 0;
        mutable shared_mutex mtx;
    };
    Slot slots[M];

public:
    int get(int i) const {
        shared_lock lk(slots[i].mtx);
        return slots[i].value;
    }

    void set(int i, int v) {
        unique_lock lk(slots[i].mtx);
        slots[i].value = v;
    }

    operator string() const {
        int v0, v1, v2;
        {
            shared_lock l0(slots[0].mtx);
            shared_lock l1(slots[1].mtx);
            shared_lock l2(slots[2].mtx);
            v0 = slots[0].value;
            v1 = slots[1].value;
            v2 = slots[2].value;
        } // м'ютекси відпущено до формування рядка
        return to_string(v0) + " " + to_string(v1) + " " + to_string(v2);
    }
};

// ---------------------------------------------------------------------------
// Тестові дані
// ---------------------------------------------------------------------------
enum class OpType : uint8_t { Read, Write, String };

struct Op {
    OpType type;
    uint8_t field;
    int value;
};

// Порядок ваг: read0, write0, read1, write1, read2, write2, string
using Weights = array<double, 7>;

void generateFile(const string& path, const Weights& w, unsigned seed) {
    mt19937 gen(seed);
    discrete_distribution<int> dist(w.begin(), w.end());
    ofstream out(path);
    for (int i = 0; i < CHUNK_OPS; i++) {
        switch (dist(gen)) {
        case 0: out << "read 0\n"; break;
        case 1: out << "write 0 1\n"; break;
        case 2: out << "read 1\n"; break;
        case 3: out << "write 1 1\n"; break;
        case 4: out << "read 2\n"; break;
        case 5: out << "write 2 1\n"; break;
        default: out << "string\n"; break;
        }
    }
}

vector<Op> loadFile(const string& path) {
    vector<Op> ops;
    ops.reserve(CHUNK_OPS);
    ifstream in(path);
    string cmd;
    while (in >> cmd) {
        if (cmd == "read") {
            int f;
            in >> f;
            ops.push_back(Op{ OpType::Read, static_cast<uint8_t>(f), 0 });
        }
        else if (cmd == "write") {
            int f, v;
            in >> f >> v;
            ops.push_back(Op{ OpType::Write, static_cast<uint8_t>(f), v });
        }
        else {
            ops.push_back(Op{ OpType::String, 0, 0 });
        }
    }
    return ops;
}

// Потік t отримує файли t, t+потоків, t+2*потоків ... (власні дані кожного потоку)
vector<vector<Op>> distribute(const vector<vector<Op>>& chunks, int threads) {
    vector<vector<Op>> res(threads);
    for (size_t c = 0; c < chunks.size(); c++) {
        auto& dst = res[c % threads];
        dst.insert(dst.end(), chunks[c].begin(), chunks[c].end());
    }
    return res;
}

// ---------------------------------------------------------------------------
// Виконання
// ---------------------------------------------------------------------------
void worker(SharedData& data, const vector<Op>& ops, long long& sink) {
    long long acc = 0;
    for (const Op& op : ops) {
        switch (op.type) {
        case OpType::Read:
            acc += data.get(op.field);
            break;
        case OpType::Write:
            data.set(op.field, op.value);
            break;
        case OpType::String: {
            string s = data;
            acc += static_cast<long long>(s.size());
            break;
        }
        }
    }
    sink = acc;
}

double runOnce(const vector<vector<Op>>& perThread) {
    SharedData data;
    vector<long long> sinks(perThread.size(), 0);
    vector<thread> threads;

    auto start = chrono::steady_clock::now();
    for (size_t i = 0; i < perThread.size(); i++)
        threads.emplace_back(worker, ref(data), cref(perThread[i]), ref(sinks[i]));
    for (auto& t : threads)
        t.join();
    auto end = chrono::steady_clock::now();

    return chrono::duration<double, milli>(end - start).count();
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    namespace fs = std::filesystem;
    fs::create_directory("tests");

    const array<char, 3> names = { 'a', 'b', 'c' };
    const array<Weights, 3> weights = { {
        {5, 5, 30, 5, 24, 1, 30}, // а) частоти з умови варіанта 7
        {1, 1, 1, 1, 1, 1, 1},    // б) усі частоти рівні
        {40, 50, 1, 1, 1, 1, 6}   // в) сильно не відповідає умові
    } };

    // 1. Генерація файлів (якщо їх ще немає)
    for (int s = 0; s < 3; s++) {
        for (int c = 0; c < CHUNKS; c++) {
            string path = "tests/" + string(1, names[s]) + "_" + to_string(c) + ".txt";
            if (!fs::exists(path)) {
                cout << "Генерація " << path << endl;
                generateFile(path, weights[s], 1000u * s + c + 1);
            }
        }
    }

    // 2. Вимірювання
    double avg[3][3];
    for (int s = 0; s < 3; s++) {
        vector<vector<Op>> chunks(CHUNKS);
        for (int c = 0; c < CHUNKS; c++)
            chunks[c] = loadFile("tests/" + string(1, names[s]) + "_" + to_string(c) + ".txt");

        for (int t = 1; t <= 3; t++) {
            auto perThread = distribute(chunks, t);
            size_t total = 0;
            for (auto& v : perThread) total += v.size();

            cout << "Сценарій " << names[s] << ", потоків: " << t
                << ", усього операцій: " << total << endl;
            cout << "  запуски, мс:";
            double sum = 0;
            for (int r = 0; r < RUNS; r++) {
                double ms = runOnce(perThread);
                sum += ms;
                cout << " " << static_cast<long long>(ms);
            }
            avg[s][t - 1] = sum / RUNS;
            cout << "  | середнє: " << static_cast<long long>(avg[s][t - 1]) << " мс" << endl;
        }
    }

    // 3. Підсумкова таблиця 3x3 (середній час, мс)
    cout << "\nСередній час виконання, мс (рядки: файли a, b, c; стовпці: 1, 2, 3 потоки)\n";
    cout << "\t1 потік\t2 потоки\t3 потоки\n";
    for (int s = 0; s < 3; s++) {
        cout << names[s];
        for (int t = 0; t < 3; t++)
            cout << "\t" << static_cast<long long>(avg[s][t]);
        cout << "\n";
    }
    return 0;
}