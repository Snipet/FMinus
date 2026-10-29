#include "tone_lib.cpp"
int main()
{
    for (double fs : { 48000.0, 96000.0, 192000.0 })
        for (double T : { 1e-4, 0.5, 1 - 1e-4 })
        {
            double w5 = 0, w10 = 0, w20 = 0;
            for (double f = 20; f <= 20000; f *= 1.01)
            {
                const cd s(0, 2 * M_PI * f);
                const double d = std::fabs(20 * std::log10(std::abs(discrete(f, fs, T)) / std::abs(netlist(s, T * Rp, (1 - T) * Rp))));
                if (f <= 5000) w5 = std::fmax(w5, d);
                if (f <= 10000) w10 = std::fmax(w10, d);
                w20 = std::fmax(w20, d);
            }
            std::printf("TR at %6.0f Hz, T = %.4f: worst |error| to 5 kHz %.3f dB, to 10 kHz %.3f dB, to 20 kHz %.3f dB\n", fs, T, w5, w10, w20);
        }
}
