#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <cstdio>   // for std::remove

constexpr int ROWS = 50000;
constexpr int ITERATIONS = 5;

long long run_task(int rows = ROWS) {
    const std::string filename = "temp_test.csv";

    // ---- Write CSV (I/O) ----
    {
        std::ofstream out(filename);
        out << "id,value\n";
        for (int i = 0; i < rows; ++i) {
            out << i << "," << (i * 13) % 10000 << "\n";
        }
    } // file closed here

    // ---- Read CSV (I/O) + small processing ----
    std::ifstream in(filename);
    std::string line;
    long long total = 0;

    // Skip header
    std::getline(in, line);

    while (std::getline(in, line)) {
        auto comma = line.find(',');
        int value = std::stoi(line.substr(comma + 1));
        total += value;
    }

    in.close();
    std::remove(filename.c_str());

    return total;
}

int main() {
    using clock = std::chrono::high_resolution_clock;

    double total_time = 0.0;

    for (int i = 0; i < ITERATIONS; ++i) {
        auto start = clock::now();
        long long checksum = run_task();
        auto end = clock::now();

        std::chrono::duration<double> elapsed = end - start;
        total_time += elapsed.count();

        std::cout << "Run " << (i + 1)
                  << ": " << elapsed.count()
                  << " seconds (checksum=" << checksum << ")\n";
    }

    std::cout << "\nAverage execution time: "
              << (total_time / ITERATIONS)
              << " seconds\n";

    return 0;
}
