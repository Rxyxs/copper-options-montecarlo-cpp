// Copper Options Monte Carlo -- high-throughput Asian-option pricer.
//
// Zero external dependencies: C++17 standard library only (<random>,
// <thread>, <chrono>). Builds directly with cl.exe (see build.ps1).
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "AsianOption.h"
#include "ClosedFormAsian.h"
#include "MarketModel.h"
#include "MonteCarloEngine.h"

namespace {

struct CliOptions {
    MarketParams mp{};
    AsianOptionSpec spec{};
    SimulationConfig cfg{};
    bool selfTest = false;
    bool benchmarkScaling = false;
    bool benchmarkPaths = false;
    bool showHelp = false;
};

void printHelp(const char* exe) {
    std::cout <<
        "Copper Options Monte Carlo -- Asian option pricer\n\n"
        "Usage: " << exe << " [options]\n\n"
        "Market / option parameters (illustrative defaults, not live quotes):\n"
        "  --spot F              spot copper price, USD/lb          (default 4.50)\n"
        "  --strike F            strike price, USD/lb               (default 4.50)\n"
        "  --maturity F          time to maturity, years             (default 1.0)\n"
        "  --vol F               annualized volatility               (default 0.28)\n"
        "  --rate F              risk-free rate                      (default 0.045)\n"
        "  --convenience F       convenience yield (GBM only)        (default 0.02)\n"
        "  --kappa F             mean-reversion speed (Schwartz)     (default 1.2)\n"
        "  --theta-level F       equilibrium price level (Schwartz)  (default 4.30)\n"
        "  --averaging-points N  number of fixings                   (default 252)\n"
        "  --type call|put                                           (default call)\n"
        "  --model gbm|schwartz|heston                               (default schwartz)\n"
        "  --heston-v0 F         initial variance                    (default 0.0784)\n"
        "  --heston-kappa-v F    variance mean-reversion speed        (default 2.0)\n"
        "  --heston-theta-v F    long-run variance                   (default 0.0784)\n"
        "  --heston-xi F         vol-of-vol                           (default 0.35)\n"
        "  --heston-rho F        price/variance correlation           (default -0.55)\n\n"
        "Simulation controls:\n"
        "  --paths N             number of Monte Carlo paths         (default 4000000)\n"
        "  --threads N           1 = sequential, else parallel        (default 0, parallel)\n"
        "  --seed N              base RNG seed                       (default 42)\n"
        "  --no-antithetic       disable antithetic variates\n"
        "  --no-control-variate  disable geometric control variate\n"
        "  --self-test           validate engine vs. closed form / parity\n"
        "  --benchmark-scaling   also run single-threaded, report measured speedup\n"
        "  --benchmark-paths     report elapsed ms across a path-count sweep\n"
        "  --help                show this message\n";
}

bool parseArgs(int argc, char** argv, CliOptions& opt) {
    // Illustrative example parameters, not a live market feed.
    opt.mp = MarketParams{/*S0*/ 4.50, /*r*/ 0.045, /*q*/ 0.02, /*sigma*/ 0.28,
                           /*kappa*/ 1.2, /*theta*/ std::log(4.30),
                           ModelType::SchwartzMeanReverting};
    opt.spec = AsianOptionSpec{/*strike*/ 4.50, /*maturity*/ 1.0, /*numAveragingPoints*/ 252,
                                OptionType::Call, AveragingType::Arithmetic};
    opt.cfg = SimulationConfig{};
    opt.cfg.numPaths = 4'000'000;

    double thetaLevel = 4.30;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto nextDouble = [&](double& out) { out = std::stod(argv[++i]); };
        auto nextSize = [&](size_t& out) { out = static_cast<size_t>(std::stoull(argv[++i])); };

        if (a == "--spot") nextDouble(opt.mp.S0);
        else if (a == "--strike") nextDouble(opt.spec.strike);
        else if (a == "--maturity") nextDouble(opt.spec.maturity);
        else if (a == "--vol") nextDouble(opt.mp.sigma);
        else if (a == "--rate") nextDouble(opt.mp.r);
        else if (a == "--convenience") nextDouble(opt.mp.q);
        else if (a == "--kappa") nextDouble(opt.mp.kappa);
        else if (a == "--theta-level") { nextDouble(thetaLevel); opt.mp.theta = std::log(thetaLevel); }
        else if (a == "--averaging-points") nextSize(opt.spec.numAveragingPoints);
        else if (a == "--paths") nextSize(opt.cfg.numPaths);
        else if (a == "--threads") opt.cfg.numThreads = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (a == "--seed") opt.cfg.seed = static_cast<uint64_t>(std::stoull(argv[++i]));
        else if (a == "--type") {
            const std::string v = argv[++i];
            opt.spec.type = (v == "put") ? OptionType::Put : OptionType::Call;
        } else if (a == "--model") {
            const std::string v = argv[++i];
            if (v == "gbm") opt.mp.model = ModelType::GeometricBrownianMotion;
            else if (v == "heston") opt.mp.model = ModelType::Heston;
            else opt.mp.model = ModelType::SchwartzMeanReverting;
        } else if (a == "--heston-v0") nextDouble(opt.mp.v0);
        else if (a == "--heston-kappa-v") nextDouble(opt.mp.kappaV);
        else if (a == "--heston-theta-v") nextDouble(opt.mp.thetaV);
        else if (a == "--heston-xi") nextDouble(opt.mp.xiV);
        else if (a == "--heston-rho") nextDouble(opt.mp.rho);
        else if (a == "--no-antithetic") opt.cfg.antithetic = false;
        else if (a == "--no-control-variate") opt.cfg.controlVariate = false;
        else if (a == "--self-test") opt.selfTest = true;
        else if (a == "--benchmark-scaling") opt.benchmarkScaling = true;
        else if (a == "--benchmark-paths") opt.benchmarkPaths = true;
        else if (a == "--help" || a == "-h") { opt.showHelp = true; return true; }
        else {
            std::cerr << "Unknown option: " << a << "\n";
            return false;
        }
    }
    return true;
}

std::string modelName(ModelType m) {
    switch (m) {
        case ModelType::GeometricBrownianMotion: return "GBM";
        case ModelType::Heston: return "Heston stochastic-volatility";
        default: return "Schwartz mean-reverting";
    }
}

void printResult(const char* label, const SimulationResult& r) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "  " << label << " price        : " << r.price << " USD\n";
    std::cout << "  Std. error          : " << r.stdError << "\n";
    std::cout << "  95% CI              : [" << (r.price - r.confInterval95) << ", "
              << (r.price + r.confInterval95) << "]\n";
    std::cout << std::setprecision(3);
    std::cout << "  Paths / threads     : " << r.numPaths << " / " << r.numThreads << "\n";
    std::cout << "  Elapsed             : " << r.elapsedSeconds << " s\n";
    std::cout << std::setprecision(0);
    std::cout << "  Throughput          : " << r.pathsPerSecond << " paths/sec\n";
}

int runSelfTest(const MarketParams& mpIn, size_t numAveragingPoints, uint64_t seed) {
    std::cout << "=== Self-test: Monte Carlo vs. Kemna-Vorst closed form ===\n";
    std::cout << "(GBM model, geometric-average option -- has a known analytic price)\n\n";

    MarketParams mp = mpIn;
    mp.model = ModelType::GeometricBrownianMotion;

    AsianOptionSpec spec{/*strike*/ mp.S0, /*maturity*/ 1.0, numAveragingPoints, OptionType::Call,
                          AveragingType::Geometric};

    const double closedForm = ClosedFormAsian::geometricAsianPrice(mp, spec);

    SimulationConfig cfg;
    cfg.numPaths = 2'000'000;
    cfg.controlVariate = false;  // nothing to control against for a geometric target
    cfg.seed = seed;

    const SimulationResult mc = MonteCarloEngine::price(mp, spec, cfg);

    const double diff = std::abs(mc.price - closedForm);
    const double tolerance = 4.0 * mc.stdError;  // 4 standard errors
    const bool pass = diff <= tolerance;

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "  Closed-form price   : " << closedForm << " USD\n";
    std::cout << "  Monte Carlo price   : " << mc.price << " USD (stderr " << mc.stdError << ")\n";
    std::cout << "  |difference|        : " << diff << " (tolerance " << tolerance << ")\n";
    std::cout << "  Result              : " << (pass ? "PASS" : "FAIL") << "\n\n";

    return pass ? 0 : 1;
}

// Heston has no simple closed-form arithmetic-Asian price to check against,
// so its self-test uses a model-agnostic identity instead: put-call parity
// for an arithmetic-average option. Under *any* risk-neutral diffusion
// where S_t remains a martingale after discounting (true for GBM, Schwartz,
// and Heston alike -- stochastic volatility changes the *distribution* of
// S_t but not its risk-neutral expectation), E[S_t] = S0 * e^{(r-q)t}, so
// the expected arithmetic average is a model-independent closed form:
//   E[Average] = (1/N) * sum_i S0 * e^{(r-q) * t_i}
// and therefore  Call - Put = e^{-rT} * (E[Average] - K)  exactly, for any
// of the three models. This is a genuine correctness check on the Heston
// path simulator and the payoff/discounting logic, not a weaker one just
// because Heston lacks an analytic option price.
int runHestonParityTest(const MarketParams& mpIn, const AsianOptionSpec& specIn, uint64_t seed) {
    std::cout << "=== Self-test: Heston put-call parity (arithmetic average) ===\n";
    std::cout << "(model-independent identity: works for any risk-neutral diffusion)\n\n";

    MarketParams mp = mpIn;
    mp.model = ModelType::Heston;

    double forwardAvgSum = 0.0;
    const double dt = specIn.maturity / static_cast<double>(specIn.numAveragingPoints);
    for (size_t i = 1; i <= specIn.numAveragingPoints; ++i) {
        const double t = dt * static_cast<double>(i);
        forwardAvgSum += mp.S0 * std::exp((mp.r - mp.q) * t);
    }
    const double forwardAvg = forwardAvgSum / static_cast<double>(specIn.numAveragingPoints);
    const double discount = std::exp(-mp.r * specIn.maturity);
    const double expectedDiff = discount * (forwardAvg - specIn.strike);

    AsianOptionSpec callSpec = specIn;
    callSpec.type = OptionType::Call;
    AsianOptionSpec putSpec = specIn;
    putSpec.type = OptionType::Put;

    SimulationConfig cfg;
    cfg.numPaths = 2'000'000;
    cfg.controlVariate = false;  // no geometric anchor available for Heston (see MonteCarloEngine)
    cfg.seed = seed;

    const SimulationResult callResult = MonteCarloEngine::price(mp, callSpec, cfg);
    const SimulationResult putResult = MonteCarloEngine::price(mp, putSpec, cfg);
    const double mcDiff = callResult.price - putResult.price;

    // Independent draws for call/put, so combined stderr adds in quadrature.
    const double combinedStdErr =
        std::sqrt(callResult.stdError * callResult.stdError + putResult.stdError * putResult.stdError);
    const double diff = std::abs(mcDiff - expectedDiff);
    const double tolerance = 4.0 * combinedStdErr;
    const bool pass = diff <= tolerance;

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "  Call price          : " << callResult.price << " USD\n";
    std::cout << "  Put price           : " << putResult.price << " USD\n";
    std::cout << "  MC   Call - Put     : " << mcDiff << "\n";
    std::cout << "  Parity Call - Put   : " << expectedDiff << " (model-independent)\n";
    std::cout << "  |difference|        : " << diff << " (tolerance " << tolerance << ")\n";
    std::cout << "  Result              : " << (pass ? "PASS" : "FAIL") << "\n\n";

    return pass ? 0 : 1;
}

void runPathBenchmark(const MarketParams& mp, const AsianOptionSpec& spec, uint64_t seed) {
    // Saves/restores stream formatting state explicitly: leaving a stray
    // `setprecision(0)` active after this function was a real bug caught by
    // actually running the program end to end -- it silently truncated
    // every subsequent floating-point print (e.g. "Spot / Strike : 4 / 4"
    // instead of "4.5 / 4.5") for the rest of the run.
    std::ios::fmtflags savedFlags(std::cout.flags());
    std::streamsize savedPrecision = std::cout.precision();

    std::cout << "=== Benchmark: elapsed time vs. path count ===\n";
    std::cout << "(std::execution::par_unseq, " << std::thread::hardware_concurrency()
              << " hardware threads)\n\n";
    std::cout << std::right << std::setw(14) << "Paths" << std::setw(16) << "Elapsed (ms)"
              << std::setw(24) << "Throughput (paths/s)" << "\n";

    const size_t pathCounts[] = {100'000, 500'000, 1'000'000, 2'000'000, 4'000'000, 8'000'000};
    for (size_t n : pathCounts) {
        SimulationConfig cfg;
        cfg.numPaths = n;
        cfg.seed = seed;
        const SimulationResult r = MonteCarloEngine::price(mp, spec, cfg);
        std::cout << std::right << std::setw(14) << n << std::setw(16) << std::fixed
                  << std::setprecision(1) << (r.elapsedSeconds * 1000.0) << std::setw(24)
                  << std::setprecision(0) << r.pathsPerSecond << "\n";
    }
    std::cout << "\n";

    std::cout.flags(savedFlags);
    std::cout.precision(savedPrecision);
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions opt;
    if (!parseArgs(argc, argv, opt)) {
        printHelp(argv[0]);
        return 1;
    }
    if (opt.showHelp) {
        printHelp(argv[0]);
        return 0;
    }

    int selfTestStatus = 0;
    if (opt.selfTest) {
        selfTestStatus = runSelfTest(opt.mp, opt.spec.numAveragingPoints, opt.cfg.seed);
        const int hestonStatus = runHestonParityTest(opt.mp, opt.spec, opt.cfg.seed);
        selfTestStatus = (selfTestStatus != 0) ? selfTestStatus : hestonStatus;
    }

    if (opt.benchmarkPaths) {
        runPathBenchmark(opt.mp, opt.spec, opt.cfg.seed);
    }

    // Both self-tests print with std::fixed + a nonstandard precision and
    // never reset it, so without this the header block below would inherit
    // that formatting (e.g. "4.500000" instead of "4.5") whenever
    // --self-test was also passed.
    std::cout.unsetf(std::ios::floatfield);
    std::cout.precision(6);

    std::cout << "=== Copper Asian Option -- Monte Carlo Pricing ===\n\n";
    std::cout << "Model               : " << modelName(opt.mp.model) << "\n";
    std::cout << "Spot / Strike       : " << opt.mp.S0 << " / " << opt.spec.strike << " USD/lb\n";
    std::cout << "Maturity            : " << opt.spec.maturity << " years, "
              << opt.spec.numAveragingPoints << " averaging fixings\n";
    std::cout << "Volatility / Rate   : " << opt.mp.sigma << " / " << opt.mp.r << "\n";
    std::cout << "Type                : " << (opt.spec.type == OptionType::Call ? "Call" : "Put")
              << " (arithmetic average)\n";
    const bool cvActuallyUsed = opt.cfg.controlVariate && opt.mp.model != ModelType::Heston;
    std::cout << "Antithetic / CV     : " << (opt.cfg.antithetic ? "on" : "off") << " / "
              << (cvActuallyUsed ? "on" : (opt.mp.model == ModelType::Heston ? "off (n/a for Heston)"
                                                                              : "off"))
              << "\n";
    std::cout << "Hardware threads    : " << std::thread::hardware_concurrency() << "\n\n";

    const SimulationResult full = MonteCarloEngine::price(opt.mp, opt.spec, opt.cfg);
    printResult("Asian option", full);

    if (opt.benchmarkScaling) {
        std::cout << "\n=== Thread scaling benchmark ===\n";
        SimulationConfig singleCfg = opt.cfg;
        singleCfg.numThreads = 1;
        const SimulationResult single = MonteCarloEngine::price(opt.mp, opt.spec, singleCfg);

        std::cout << "  1 thread            : " << single.pathsPerSecond << " paths/sec ("
                  << single.elapsedSeconds << " s)\n";
        std::cout << "  " << full.numThreads << " threads           : " << full.pathsPerSecond
                   << " paths/sec (" << full.elapsedSeconds << " s)\n";
        std::cout << std::setprecision(2);
        std::cout << "  Measured speedup    : " << (full.pathsPerSecond / single.pathsPerSecond)
                  << "x\n";
    }

    return selfTestStatus;
}
