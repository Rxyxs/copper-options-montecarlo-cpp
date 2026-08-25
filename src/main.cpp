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
        "  --model gbm|schwartz                                      (default schwartz)\n\n"
        "Simulation controls:\n"
        "  --paths N             number of Monte Carlo paths         (default 4000000)\n"
        "  --threads N           worker threads, 0 = all cores       (default 0)\n"
        "  --seed N              base RNG seed                       (default 42)\n"
        "  --no-antithetic       disable antithetic variates\n"
        "  --no-control-variate  disable geometric control variate\n"
        "  --self-test           validate engine vs. Kemna-Vorst closed form\n"
        "  --benchmark-scaling   also run single-threaded, report measured speedup\n"
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
            opt.mp.model = (v == "gbm") ? ModelType::GeometricBrownianMotion
                                         : ModelType::SchwartzMeanReverting;
        } else if (a == "--no-antithetic") opt.cfg.antithetic = false;
        else if (a == "--no-control-variate") opt.cfg.controlVariate = false;
        else if (a == "--self-test") opt.selfTest = true;
        else if (a == "--benchmark-scaling") opt.benchmarkScaling = true;
        else if (a == "--help" || a == "-h") { opt.showHelp = true; return true; }
        else {
            std::cerr << "Unknown option: " << a << "\n";
            return false;
        }
    }
    return true;
}

std::string modelName(ModelType m) {
    return m == ModelType::GeometricBrownianMotion ? "GBM" : "Schwartz mean-reverting";
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
    }

    std::cout << "=== Copper Asian Option -- Monte Carlo Pricing ===\n\n";
    std::cout << "Model               : " << modelName(opt.mp.model) << "\n";
    std::cout << "Spot / Strike       : " << opt.mp.S0 << " / " << opt.spec.strike << " USD/lb\n";
    std::cout << "Maturity            : " << opt.spec.maturity << " years, "
              << opt.spec.numAveragingPoints << " averaging fixings\n";
    std::cout << "Volatility / Rate   : " << opt.mp.sigma << " / " << opt.mp.r << "\n";
    std::cout << "Type                : " << (opt.spec.type == OptionType::Call ? "Call" : "Put")
              << " (arithmetic average)\n";
    std::cout << "Antithetic / CV     : " << (opt.cfg.antithetic ? "on" : "off") << " / "
              << (opt.cfg.controlVariate ? "on" : "off") << "\n";
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
