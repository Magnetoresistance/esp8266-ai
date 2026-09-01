using NAudio.Dsp;
using NAudio.Wave;
using System.Text.Json;

namespace AIClockBridge;

// Captures the system's audio output (WASAPI loopback ?? whatever this PC is
// playing, from any app) and computes a small log-spaced magnitude spectrum
// for the ESP8266's music screen. The device polls /music/spectrum ~10x/s
// and draws the bars in one of several styles (firmware-side). No audio is
// recorded or stored ?? only 24 bar magnitudes leave this machine.
sealed class SpectrumService : IDisposable
{
    const int FftSize = 1024;       // 2^10 samples per FFT
    const int BarCount = 24;        // log-spaced bars across ~40Hz-16kHz
    const int LogN = 10;            // log2(FftSize)

    readonly object _lock = new();
    readonly byte[] _bars = new byte[BarCount];
    readonly float[] _smoothed = new float[BarCount]; // peak-hold decay
    WasapiLoopbackCapture _capture;
    float[] _samples = new float[FftSize];
    int _samplePos;
    Complex[] _fftBuf = new Complex[FftSize];
    bool _running;
    DateTime _lastDataAt = DateTime.MinValue;
    DateTime _lastSpectrumAt = DateTime.MinValue;

    public SpectrumService()
    {
        try
        {
            _capture = new WasapiLoopbackCapture();
            _capture.DataAvailable += OnData;
            _capture.RecordingStopped += OnStopped;
            _capture.StartRecording();
            _running = true;
            Console.Error.WriteLine($"[spectrum] listening {_capture.WaveFormat.SampleRate}Hz {_capture.WaveFormat.BitsPerSample}bit");
        }
        catch (Exception e)
        {
            _running = false;
            Console.Error.WriteLine($"[spectrum] capture failed: {e.Message}");
        }
    }

    /// Latest 24 bar magnitudes (0-255), for the mirror popup preview.
    public byte[] Bars
    {
        get
        {
            lock (_lock)
            {
                var src = EffectiveBars();
                var copy = new byte[BarCount];
                for (int i = 0; i < BarCount; i++) copy[i] = (byte)src[i];
                return copy;
            }
        }
    }

    /// Effective bars with the silence guard applied (see silence comment
    /// below): shared by both the device JSON and the preview's Bars getter
    /// so the popup fades out exactly like the device.
    int[] EffectiveBars()
    {
        // WASAPI loopback keeps sending near-empty buffers after audio
        // stops, but they never fill the 1024-sample FFT window so
        // ComputeSpectrum stops being called.  When no full FFT frame
        // has been computed for > 0.3s, return zeros so the device's
        // own time-smoothing handles the fade-out (continuous 27%/frame
        // blend) instead of a choppy bridge-side step decay.
        bool silent = _lastSpectrumAt != DateTime.MinValue &&
            (DateTime.UtcNow - _lastSpectrumAt).TotalSeconds > 0.3;
        var bars = new int[BarCount];
        if (!silent)
            for (int i = 0; i < BarCount; i++) bars[i] = _bars[i];
        return bars;
    }

    /// Latest 24 bar magnitudes (0-255, log-spaced low??high), JSON for the
    /// device: {"bars":[..],"running":true|false}. Bars are copied to an
    /// int[] because System.Text.Json would serialize byte[] as Base64.
    public byte[] ToJson()
    {
        lock (_lock)
        {
            var bars = EffectiveBars();
            var json = JsonSerializer.Serialize(new { bars, running = _running });
            return System.Text.Encoding.UTF8.GetBytes(json);
        }
    }

    void OnData(object sender, WaveInEventArgs e)
    {
        lock (_lock)
        {
            _lastDataAt = DateTime.UtcNow;
            int bytesPerSample = _capture.WaveFormat.BitsPerSample / 8;
            int channels = _capture.WaveFormat.Channels;
            int frameBytes = bytesPerSample * channels;
            for (int i = 0; i + frameBytes - 1 < e.BytesRecorded; i += frameBytes)
            {
                short sample = BitConverter.ToInt16(e.Buffer, i);
                _samples[_samplePos++] = sample / 32768f;
                if (_samplePos >= FftSize)
                {
                    ComputeSpectrum();
                    _samplePos = 0;
                }
            }
        }
    }

    void ComputeSpectrum()
    {
        _lastSpectrumAt = DateTime.UtcNow;
        // Hanning window + copy into FFT buffer
        for (int i = 0; i < FftSize; i++)
        {
            double w = 0.5 - 0.5 * Math.Cos(2.0 * Math.PI * i / (FftSize - 1));
            _fftBuf[i].X = (float)(_samples[i] * w);
            _fftBuf[i].Y = 0;
        }
        FastFourierTransform.FFT(true, LogN, _fftBuf);

        int sampleRate = _capture.WaveFormat.SampleRate;
        double fMin = 40, fMax = 16000;
        for (int b = 0; b < BarCount; b++)
        {
            double f0 = fMin * Math.Pow(fMax / fMin, (double)b / BarCount);
            double f1 = fMin * Math.Pow(fMax / fMin, (double)(b + 1) / BarCount);
            int i0 = Math.Max(1, (int)(f0 / sampleRate * FftSize));
            int i1 = Math.Min(FftSize / 2 - 1, (int)(f1 / sampleRate * FftSize));
            double sum = 0;
            for (int i = i0; i <= i1; i++)
            {
                double mag = Math.Sqrt(_fftBuf[i].X * _fftBuf[i].X + _fftBuf[i].Y * _fftBuf[i].Y);
                sum += mag;
            }
            int n = Math.Max(1, i1 - i0 + 1);
            double avg = sum / n;
            // dB-scaled magnitude: -60..0 dB maps to 0..255 (silence ?? 0,
            // loud music saturates near the top; linear scaling saturated
            // everything because FFT bin magnitudes run large).
            double db = 20.0 * Math.Log10(avg + 1e-9);
            float target = (float)Math.Clamp((db + 60.0) / 60.0, 0.0, 1.0) * 255.0f;

            // peak-hold with slow decay (smooth, lively falloff)
            if (target >= _smoothed[b]) _smoothed[b] = target;
            else _smoothed[b] = _smoothed[b] * 0.85f + target * 0.15f;
            _bars[b] = (byte)Math.Clamp((int)_smoothed[b], 0, 255);
        }
        // silence gate: if the loudest bar is below threshold the machine is
        // effectively silent ?? zero the whole frame so the device draws no
        // phantom spectrum (most visible with auto-range normalization).
        byte peak = 0;
        for (int b = 0; b < BarCount; b++) if (_bars[b] > peak) peak = _bars[b];
        if (peak < 6) Array.Clear(_bars, 0, BarCount);
    }

    void OnStopped(object sender, StoppedEventArgs e)
    {
        _running = false;
        Console.Error.WriteLine("[spectrum] capture stopped");
    }

    public void Dispose()
    {
        try { _capture?.StopRecording(); } catch { }
        try { _capture?.Dispose(); } catch { }
    }
}
