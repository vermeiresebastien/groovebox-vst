#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace gb
{

/** Precomputed windowed-sinc coefficient table.

    Borrowed from redrum's engine (`redrum/Source/Engine/Resampler.h`), which
    already solved this problem: a pitch change moves a sample's top end above
    the output's Nyquist, and the only choices are to fold it back down or to
    filter it out. Linear interpolation folds, and at a ratio of exactly two it
    does not even interpolate — it decimates, so everything above half the source
    rate comes back into the audible band untouched.

    The table has several cutoff bands so an upward pitch shift lowers the kernel's
    cutoff below Nyquist and suppresses the alias instead of merely smoothing it. A
    voice picks one band at note-on, so the kernel never changes within a hit.

    Construction is deliberately done when the engine is constructed, off the audio
    thread. No allocation or trigonometry happens in the render loop.
*/
class SincKernelTable
{
public:
    static constexpr int taps = 24;
    static constexpr int halfWidth = taps / 2;
    static constexpr int phases = 1024;
    static constexpr int cutoffBands = 16;

    SincKernelTable()
    {
        constexpr auto pi = 3.14159265358979323846;
        coefficients.resize ((std::size_t) cutoffBands * (std::size_t) (phases + 1)
                             * (std::size_t) taps);

        for (int band = 0; band < cutoffBands; ++band)
        {
            const auto cutoff = cutoffForBand (band);

            for (int phase = 0; phase <= phases; ++phase)
            {
                const auto fraction = (double) phase / (double) phases;
                double values[taps] {};
                double sum = 0.0;

                for (int tap = 0; tap < taps; ++tap)
                {
                    // Base tap indices run from (floor - 11) to (floor + 12),
                    // so tap 11 is the source sample immediately below the
                    // fractional read position.
                    const auto distance = (double) (tap - (halfWidth - 1)) - fraction;
                    const auto x = cutoff * distance;
                    const auto sinc = std::abs (x) < 1.0e-12
                                          ? 1.0
                                          : std::sin (pi * x) / (pi * x);
                    const auto windowPosition = distance / (double) halfWidth;
                    const auto window = std::abs (windowPosition) >= 1.0
                                            ? 0.0
                                            : 0.42 + 0.5 * std::cos (pi * windowPosition)
                                                  + 0.08 * std::cos (2.0 * pi * windowPosition);
                    const auto value = cutoff * sinc * window;
                    values[(std::size_t) tap] = value;
                    sum += value;
                }

                // Normalising to unity DC keeps a resampled hit at the level the
                // source was recorded at, so the kernel cannot become a gain change.
                if (std::abs (sum) > 1.0e-12)
                    for (auto& value : values)
                        value /= sum;

                for (int tap = 0; tap < taps; ++tap)
                    at (band, phase, tap) = (float) values[(std::size_t) tap];
            }
        }
    }

    /** Kernel cutoff for a band, as a fraction of the source Nyquist. */
    [[nodiscard]] static float cutoffForBand (int band) noexcept
    {
        const auto normalised = (float) std::clamp (band, 0, cutoffBands - 1)
                                / (float) (cutoffBands - 1);
        return 0.18f + normalised * 0.82f;
    }

    /** Band to read a source at `ratio` with, chosen once per hit.

        A ratio above one moves source content up by that factor, so the cutoff
        comes down by the same amount and the 0.96 leaves a little margin for the
        transition band. Reading slower than real time needs no filtering: the
        source's own bandwidth is already inside what the output can carry.
    */
    [[nodiscard]] static int bandForRatio (double ratio) noexcept
    {
        const auto safeRatio = std::max (0.001, ratio);
        const auto cutoff = std::clamp (0.96 / std::max (1.0, safeRatio), 0.18, 1.0);
        return std::clamp ((int) std::lround ((cutoff - 0.18f) / 0.82f * (float) (cutoffBands - 1)),
                           0,
                           cutoffBands - 1);
    }

    /** Fills `weights` with `taps` normalised coefficients. */
    void getWeights (int band, double fraction, float* weights) const noexcept
    {
        const auto clampedBand = std::clamp (band, 0, cutoffBands - 1);
        const auto position = std::clamp (fraction, 0.0, 0.999999999) * (double) phases;
        const auto phase0 = (int) position;
        const auto phase1 = std::min (phase0 + 1, phases);
        const auto blend = (float) (position - (double) phase0);

        for (int tap = 0; tap < taps; ++tap)
        {
            const auto a = at (clampedBand, phase0, tap);
            const auto b = at (clampedBand, phase1, tap);
            weights[tap] = a + (b - a) * blend;
        }
    }

private:
    [[nodiscard]] float& at (int band, int phase, int tap) noexcept
    {
        return coefficients[((std::size_t) band * (std::size_t) (phases + 1) + (std::size_t) phase)
                            * (std::size_t) taps + (std::size_t) tap];
    }

    [[nodiscard]] const float& at (int band, int phase, int tap) const noexcept
    {
        return coefficients[((std::size_t) band * (std::size_t) (phases + 1) + (std::size_t) phase)
                            * (std::size_t) taps + (std::size_t) tap];
    }

    std::vector<float> coefficients;
};

} // namespace gb
