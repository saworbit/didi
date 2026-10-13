# Pin each fixture game while it is alive. A later PID lookup may name a
# different process, so the handshake precedes every scenario's first step.
if (-not ('DidiScenarioProcessWitness' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;

public sealed class DidiScenarioProcessWitness : IDisposable {
    public readonly ConcurrentDictionary<int, Process> Processes = new ConcurrentDictionary<int, Process>();
    public Exception Error { get; private set; }
    private readonly string directory;
    private readonly Thread reader;
    private volatile bool stopping;
    private bool disposed;

    public DidiScenarioProcessWitness(string fixtureRoot) {
        directory = Path.Combine(Path.GetFullPath(fixtureRoot), ".didi", "scenario-process-witness");
        if (Directory.Exists(directory)) throw new IOException("The scenario witness directory already exists.");
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "enabled"), "");
        reader = new Thread(Read);
        reader.IsBackground = true;
        try { reader.Start(); }
        catch { RemoveFiles(); throw; }
    }

    private void Read() {
        try {
            while (!stopping) {
                foreach (string request in Directory.GetFiles(directory, "*.request")) {
                    string ready = Path.ChangeExtension(request, ".ready");
                    if (File.Exists(ready)) continue;
                    int pid = int.Parse(Path.GetFileNameWithoutExtension(request), CultureInfo.InvariantCulture);
                    Process process = Process.GetProcessById(pid);
                    bool kept = false;
                    try {
                        IntPtr handle = process.Handle;
                        long started = process.StartTime.ToUniversalTime().Ticks;
                        if (process.HasExited) throw new InvalidOperationException("The game exited before its witness was captured.");
                        if (!Processes.TryAdd(pid, process)) throw new InvalidOperationException("The scenario game was captured twice.");
                        kept = true;
                        File.WriteAllText(ready, started.ToString(CultureInfo.InvariantCulture));
                    } finally { if (!kept) process.Dispose(); }
                }
                Thread.Sleep(2);
            }
        } catch (Exception error) { Error = error; }
    }

    private void RemoveFiles() {
        foreach (string path in Directory.GetFiles(directory, "*.request")) File.Delete(path);
        foreach (string path in Directory.GetFiles(directory, "*.ready")) File.Delete(path);
        File.Delete(Path.Combine(directory, "enabled"));
        Directory.Delete(directory);
    }

    public void Dispose() {
        if (disposed) return;
        stopping = true;
        if (!reader.Join(5000)) throw new TimeoutException("The scenario process witness did not stop.");
        try {
            foreach (Process process in Processes.Values) process.Dispose();
            Processes.Clear();
        } finally { RemoveFiles(); disposed = true; }
    }
}
'@
}
