/**
 * main.cpp  –  AsyncWorker demonstration
 *
 * Exercises all major features:
 *   1. Task submission with input/output parameters via iovec
 *   2. Concurrent execution (multiple tasks running simultaneously)
 *   3. Pull-based status polling by task ID
 *   4. Result retrieval from the TaskDescriptor
 *   5. Task purge
 *   6. Error handling (unknown function, missing inputs)
 */

#include "async_worker.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <thread>
#include <vector>

using namespace std::chrono_literals;


// ─────────────────────────────────────────────────────────────────────────────
// Registered task functions
// ─────────────────────────────────────────────────────────────────────────────

/// "add_i32" – adds two int32_t values.
///   in[0] = int32_t a,  in[1] = int32_t b
///   out[0] = int32_t (a + b)
static int add_i32(const iovec* in,  std::size_t in_n,
                   iovec*       out, std::size_t out_n) noexcept
{
    if (in_n < 2 || out_n < 1) return -1;

    std::int32_t a{}, b{};
    std::memcpy(&a, in[0].iov_base, sizeof(a));
    std::memcpy(&b, in[1].iov_base, sizeof(b));

    const std::int32_t result = a + b;
    std::memcpy(out[0].iov_base, &result, sizeof(result));
    return 0;
}

/// "dot_product_f64" – dot product of two double arrays of equal length.
///   in[0] = double[] vec_a,  in[1] = double[] vec_b
///   out[0] = double (dot product)
static int dot_product_f64(const iovec* in,  std::size_t in_n,
                            iovec*       out, std::size_t out_n) noexcept
{
    if (in_n < 2 || out_n < 1) return -1;

    const std::size_t n = in[0].iov_len / sizeof(double);
    if (n == 0 || in[1].iov_len / sizeof(double) != n) return -2;

    const auto* a = static_cast<const double*>(in[0].iov_base);
    const auto* b = static_cast<const double*>(in[1].iov_base);

    double dot = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        dot += a[i] * b[i];

    std::memcpy(out[0].iov_base, &dot, sizeof(dot));
    return 0;
}

/// "slow_square_u32" – squares a uint32_t, simulating a slow computation.
///   in[0]  = uint32_t n
///   out[0] = uint64_t (n * n)
static int slow_square_u32(const iovec* in,  std::size_t in_n,
                            iovec*       out, std::size_t out_n) noexcept
{
    std::this_thread::sleep_for(120ms);   // simulate latency

    if (in_n < 1 || out_n < 1) return -1;

    std::uint32_t n{};
    std::memcpy(&n, in[0].iov_base, sizeof(n));

    const std::uint64_t sq = static_cast<std::uint64_t>(n) * n;
    std::memcpy(out[0].iov_base, &sq, sizeof(sq));
    return 0;
}

/// "concat_str" – concatenates two null-terminated strings.
///   in[0] = char[] prefix,  in[1] = char[] suffix
///   out[0] = char[] result  (must be large enough)
static int concat_str(const iovec* in,  std::size_t in_n,
                      iovec*       out, std::size_t out_n) noexcept
{
    if (in_n < 2 || out_n < 1) return -1;

    const auto* a = static_cast<const char*>(in[0].iov_base);
    const auto* b = static_cast<const char*>(in[1].iov_base);

    char* dst      = static_cast<char*>(out[0].iov_base);
    std::size_t cap = out[0].iov_len;

    std::size_t wa = std::strlen(a);
    std::size_t wb = std::strlen(b);

    if (wa + wb + 1 > cap) return -2;   // buffer too small

    std::memcpy(dst,      a, wa);
    std::memcpy(dst + wa, b, wb);
    dst[wa + wb] = '\0';
    return 0;
}


// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string_view status_name(async::TaskStatus s) noexcept
{
    using enum async::TaskStatus;
    switch (s) {
        case PENDING:   return "PENDING";
        case RUNNING:   return "RUNNING";
        case COMPLETED: return "COMPLETED";
    }
    return "UNKNOWN";
}

/// Spin-poll until the task reaches COMPLETED, then return the descriptor.
static std::shared_ptr<const async::TaskDescriptor>
wait_for(async::AsyncWorker& w, std::uint64_t id,
         std::chrono::milliseconds poll_interval = 10ms)
{
    while (true) {
        auto s = w.query(id);
        if (s && *s == async::TaskStatus::COMPLETED)
            return w.get(id);
        std::this_thread::sleep_for(poll_interval);
    }
}

/// Print a TaskDescriptor summary (status + return_code).
static void print_task_header(const async::TaskDescriptor& d)
{
    std::cout << std::format("  id={:<3}  fn={:<20}  status={}  rc={}\n",
                             d.id, d.function_name,
                             status_name(d.status.load(std::memory_order_relaxed)),
                             d.return_code);
}


// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    // Create a pool with 3 concurrent worker threads.
    async::AsyncWorker worker(3);

    worker.register_function("add_i32",        add_i32);
    worker.register_function("dot_product_f64", dot_product_f64);
    worker.register_function("slow_square_u32", slow_square_u32);
    worker.register_function("concat_str",      concat_str);

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  AsyncWorker demo  (3 worker threads)\n";
    std::cout << "═══════════════════════════════════════════════════\n\n";


    // ── Test 1: simple integer addition ──────────────────────────────────────
    std::cout << "── Test 1: add_i32 ─────────────────────────────────\n";
    {
        std::int32_t a = 42, b = 58;

        // Input: two int32_t scalars.
        iovec in[2] = {
            { static_cast<void*>(&a), sizeof(a) },
            { static_cast<void*>(&b), sizeof(b) },
        };
        // Output: one int32_t slot (iov_base is ignored at submit time).
        iovec out[1] = {{ nullptr, sizeof(std::int32_t) }};

        const std::uint64_t id = worker.submit({
            .function_name = "add_i32",
            .in_params  = in,  .in_count  = 2,
            .out_params = out, .out_count = 1,
        });
        std::cout << std::format("  submitted id={}\n", id);

        auto desc = wait_for(worker, id);
        print_task_header(*desc);

        std::int32_t result{};
        std::memcpy(&result, desc->out_params[0].iov_base, sizeof(result));
        std::cout << std::format("  {} + {} = {}\n\n", a, b, result);
    }


    // ── Test 2: dot product of two vectors ───────────────────────────────────
    std::cout << "── Test 2: dot_product_f64 ─────────────────────────\n";
    {
        std::vector<double> va = {1.0, 2.0, 3.0};
        std::vector<double> vb = {4.0, 5.0, 6.0};   // expected: 1*4+2*5+3*6 = 32

        iovec in[2] = {
            { va.data(), va.size() * sizeof(double) },
            { vb.data(), vb.size() * sizeof(double) },
        };
        iovec out[1] = {{ nullptr, sizeof(double) }};

        const std::uint64_t id = worker.submit({
            .function_name = "dot_product_f64",
            .in_params  = in,  .in_count  = 2,
            .out_params = out, .out_count = 1,
        });
        std::cout << std::format("  submitted id={}\n", id);

        auto desc = wait_for(worker, id);
        print_task_header(*desc);

        double dot{};
        std::memcpy(&dot, desc->out_params[0].iov_base, sizeof(dot));
        std::cout << std::format("  dot([1,2,3], [4,5,6]) = {}\n\n", dot);
    }


    // ── Test 3: five slow_square_u32 tasks running concurrently ──────────────
    std::cout << "── Test 3: slow_square_u32 (concurrent) ────────────\n";
    {
        constexpr std::uint32_t inputs[] = {3, 7, 12, 100, 255};
        std::vector<std::uint64_t> ids;

        auto t0 = std::chrono::steady_clock::now();

        for (std::uint32_t n : inputs) {
            // n is copied into each iovec immediately; safe to reuse the variable.
            iovec in[1]  = {{ static_cast<void*>(&n), sizeof(n) }};
            iovec out[1] = {{ nullptr, sizeof(std::uint64_t) }};

            ids.push_back(worker.submit({
                .function_name = "slow_square_u32",
                .in_params  = in,  .in_count  = 1,
                .out_params = out, .out_count = 1,
            }));
            std::cout << std::format("  submitted slow_square_u32({:>3}) → id={}\n",
                                     n, ids.back());
        }

        // Poll all tasks until every one is COMPLETED.
        bool all_done = false;
        while (!all_done) {
            all_done = true;
            for (auto id : ids) {
                auto s = worker.query(id);
                if (!s || *s != async::TaskStatus::COMPLETED)
                    all_done = false;
            }
            std::this_thread::sleep_for(20ms);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        std::cout << std::format("\n  All done in {}ms  (3 threads × 120ms tasks)\n", elapsed.count());

        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto desc = worker.get(ids[i]);
            std::uint64_t sq{};
            std::memcpy(&sq, desc->out_params[0].iov_base, sizeof(sq));
            std::cout << std::format("  id={}  {}² = {}  rc={}\n",
                                     ids[i], inputs[i], sq, desc->return_code);
            worker.purge(ids[i]);
        }
        std::cout << '\n';
    }


    // ── Test 4: string concatenation (multi-scalar output) ───────────────────
    std::cout << "── Test 4: concat_str ──────────────────────────────\n";
    {
        const char prefix[] = "Hello, ";
        const char suffix[] = "async world!";
        constexpr std::size_t out_size = 64;

        iovec in[2] = {
            { const_cast<char*>(prefix), sizeof(prefix) },
            { const_cast<char*>(suffix), sizeof(suffix) },
        };
        iovec out[1] = {{ nullptr, out_size }};

        const std::uint64_t id = worker.submit({
            .function_name = "concat_str",
            .in_params  = in,  .in_count  = 2,
            .out_params = out, .out_count = 1,
        });
        std::cout << std::format("  submitted id={}\n", id);

        auto desc = wait_for(worker, id);
        print_task_header(*desc);

        const auto* result = static_cast<const char*>(desc->out_params[0].iov_base);
        std::cout << std::format("  result: \"{}\"\n\n", result);
    }


    // ── Test 5: status polling demonstration ─────────────────────────────────
    std::cout << "── Test 5: status lifecycle polling ────────────────\n";
    {
        std::uint32_t n = 42;
        iovec in[1]  = {{ static_cast<void*>(&n), sizeof(n) }};
        iovec out[1] = {{ nullptr, sizeof(std::uint64_t) }};

        const std::uint64_t id = worker.submit({
            .function_name = "slow_square_u32",
            .in_params  = in,  .in_count  = 1,
            .out_params = out, .out_count = 1,
        });

        std::cout << std::format("  task id={}\n", id);

        // Poll every 30 ms and print each observed status.
        async::TaskStatus last = async::TaskStatus::PENDING;
        while (true) {
            auto s = worker.query(id);
            if (!s) break;
            if (*s != last) {
                last = *s;
                std::cout << std::format("  status changed → {}\n", status_name(*s));
            }
            if (*s == async::TaskStatus::COMPLETED) break;
            std::this_thread::sleep_for(30ms);
        }

        auto desc = worker.get(id);
        std::uint64_t sq{};
        std::memcpy(&sq, desc->out_params[0].iov_base, sizeof(sq));
        std::cout << std::format("  {}² = {}  rc={}\n\n", n, sq, desc->return_code);
    }


    // ── Test 6: query for a non-existent ID ──────────────────────────────────
    std::cout << "── Test 6: query non-existent ID ───────────────────\n";
    {
        auto s = worker.query(99999);
        std::cout << std::format("  query(99999) → {}\n\n",
                                 s ? status_name(*s) : "not found");
    }


    // ── Test 7: submit with unknown function (exception expected) ─────────────
    std::cout << "── Test 7: unknown function → exception ────────────\n";
    {
        try {
            iovec dummy_out[1] = {{ nullptr, 4 }};
            (void) worker.submit({
                .function_name = "does_not_exist",
                .out_params = dummy_out, .out_count = 1,
            });
            std::cout << "  ERROR: expected exception was not thrown\n";
        } catch (const std::runtime_error& e) {
            std::cout << std::format("  caught expected exception: {}\n\n", e.what());
        }
    }


    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << std::format("  Remaining tracked tasks: {}\n", worker.task_count());
    std::cout << "═══════════════════════════════════════════════════\n";

    return 0;
}
