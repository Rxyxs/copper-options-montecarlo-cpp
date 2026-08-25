#pragma once

// GBM: standard lognormal diffusion, used as the model with a known
// closed-form Asian (geometric-average) price -- the engine's self-test
// anchor and the control-variate reference for arithmetic pricing.
//
// SchwartzMeanReverting: single-factor Schwartz (1997) model on ln(S),
// the textbook choice for exhaustible-resource commodities like copper,
// which tend to revert to a long-run equilibrium set by marginal
// extraction cost rather than drift freely like an equity.
enum class ModelType { GeometricBrownianMotion, SchwartzMeanReverting };

struct MarketParams {
    double S0;     // spot price today (USD / lb)
    double r;      // continuously-compounded risk-free rate
    double q;      // convenience yield (cost-of-carry offset for GBM)
    double sigma;  // annualized volatility of the diffusion term
    double kappa;  // Schwartz model: speed of mean reversion
    double theta;  // Schwartz model: long-run equilibrium level of ln(S)
    ModelType model;
};
