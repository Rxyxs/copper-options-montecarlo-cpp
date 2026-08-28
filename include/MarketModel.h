#pragma once

// GBM: standard lognormal diffusion, used as the model with a known
// closed-form Asian (geometric-average) price -- the engine's self-test
// anchor and the control-variate reference for arithmetic pricing.
//
// SchwartzMeanReverting: single-factor Schwartz (1997) model on ln(S),
// the textbook choice for exhaustible-resource commodities like copper,
// which tend to revert to a long-run equilibrium set by marginal
// extraction cost rather than drift freely like an equity.
//
// Heston: Heston (1993) stochastic-volatility model -- the instantaneous
// variance V_t is itself a mean-reverting square-root (CIR) diffusion,
// correlated with the price Brownian motion via `rho` (empirically
// negative for most commodities and equities: a price drop coincides
// with a volatility spike -- the "leverage effect"). Unlike GBM and
// Schwartz, which both use an *exact* transition density (see
// PathSimulator.h), the CIR variance process has no simple exact sampler
// and is discretized here with the full-truncation Euler scheme (Lord,
// Koekkoek & van Dijk, 2010): the drift/diffusion of V use max(V, 0)
// wherever V could otherwise go negative between averaging fixings, and
// V is clamped to 0 for pricing purposes if a step lands negative. This
// is the standard, well-documented practical compromise for Heston --
// the more accurate but substantially more involved alternative is
// Andersen's Quadratic-Exponential (QE) scheme, out of scope here.
enum class ModelType { GeometricBrownianMotion, SchwartzMeanReverting, Heston };

struct MarketParams {
    double S0;     // spot price today (USD / lb)
    double r;      // continuously-compounded risk-free rate
    double q;      // convenience yield (cost-of-carry offset for GBM / Heston)
    double sigma;  // annualized volatility of the diffusion term (GBM / Schwartz)
    double kappa;  // Schwartz model: speed of mean reversion
    double theta;  // Schwartz model: long-run equilibrium level of ln(S)
    ModelType model;

    // Heston stochastic-volatility parameters. v0/thetaV are variances
    // (i.e. already squared, unlike `sigma` above), matching the
    // convention in Heston's original paper.
    double v0 = 0.0784;     // initial instantaneous variance (0.28^2)
    double kappaV = 2.0;    // speed of mean reversion of variance
    double thetaV = 0.0784; // long-run variance level
    double xiV = 0.35;      // volatility of variance ("vol-of-vol")
    double rho = -0.55;     // correlation between price and variance shocks
};
