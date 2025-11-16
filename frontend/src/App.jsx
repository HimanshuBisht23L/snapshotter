import React, { useEffect, useState } from "react";
const API_BASE = "http://127.0.0.1:8000/api";
const TOKEN = "local-secret-change-me"; // change to a strong secret in prod

export default function App() {
  const [procs, setProcs] = useState([]);
  const [saved, setSaved] = useState([]); // saved snapshots (oldpid + metadata)
  const [loading, setLoading] = useState(false);
  const [actionLoadingPid, setActionLoadingPid] = useState(null);
  const [log, setLog] = useState([]);
  const [searchPid, setSearchPid] = useState("");
  const [apiConnected, setApiConnected] = useState(null);

  const addLog = (s) =>
    setLog((l) => [new Date().toLocaleTimeString() + " — " + s, ...l].slice(0, 200));

  useEffect(() => {
    refreshAll();
    const id = setInterval(refreshAll, 5000);
    return () => clearInterval(id);
  }, []);

  async function refreshAll() {
    await Promise.all([fetchProcs(), fetchSaved()]);
  }

  async function fetchProcs() {
    setLoading(true);
    try {
      const res = await fetch(`${API_BASE}/processes`, {
        headers: { "x-snapshot-token": TOKEN },
      });
      if (!res.ok) {
        const txt = await res.text();
        throw new Error(`HTTP ${res.status}: ${txt}`);
      }
      const j = await res.json();
      setProcs(j.procs || []);
      setApiConnected(true);
      addLog(`Fetched ${j.procs?.length ?? 0} processes`);
    } catch (err) {
      setApiConnected(false);
      addLog(`fetchProcs error: ${err.message}`);
      console.error(err);
    } finally {
      setLoading(false);
    }
  }

  async function fetchSaved() {
    try {
      const res = await fetch(`${API_BASE}/saved`, { headers: { "x-snapshot-token": TOKEN }});
      if (!res.ok) {
        const txt = await res.text();
        throw new Error(`HTTP ${res.status}: ${txt}`);
      }
      const j = await res.json();
      setSaved(j.saved || []);
    } catch (e) {
      console.warn("fetchSaved failed:", e);
    }
  }

  // helper: find if pid is saved
  function isSaved(pid) {
    return saved.some((s) => s.oldpid === pid);
  }

  // Snapshot & Kill: call server (server will save metadata)
  async function snapshot(pid) {
    if (!window.confirm(`Snapshot & kill PID ${pid}?`)) return;
    setActionLoadingPid(pid);
    addLog(`Request snapshot ${pid}`);
    try {
      const res = await fetch(`${API_BASE}/snapshot`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "x-snapshot-token": TOKEN,
        },
        body: JSON.stringify({ pid }),
      });
      const j = await res.json().catch(() => ({ ok: false, raw: "invalid-json" }));
      if (!res.ok) {
        addLog(`SNAPSHOT ERR: ${JSON.stringify(j)}`);
      } else {
        addLog(`SNAPSHOT OK: ${j.out || JSON.stringify(j)}`);
        // server returned saved metadata; refresh lists
        await refreshAll();
      }
    } catch (err) {
      addLog(`SNAPSHOT network error: ${err.message}`);
      console.error(err);
    } finally {
      setActionLoadingPid(null);
    }
  }

  // Restore: request server to spawn (newpid=0). Server will return spawnedPid if succeed.
  async function restore(oldpid) {
    if (!window.confirm(`Restore saved PID ${oldpid}?`)) return;
    setActionLoadingPid(oldpid);
    addLog(`Request restore ${oldpid} -> 0`);
    try {
      const res = await fetch(`${API_BASE}/restore`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "x-snapshot-token": TOKEN,
        },
        body: JSON.stringify({ oldpid, newpid: 0 }),
      });
      const j = await res.json().catch(() => ({ ok: false, raw: "invalid-json" }));
      if (!res.ok) {
        addLog(`RESTORE ERR: ${JSON.stringify(j)}`);
      } else {
        addLog(`RESTORE OK: ${j.out || JSON.stringify(j)}`);
        let spawned = null;
        if (j.spawnedPid) spawned = Number(j.spawnedPid);
        if (!spawned && j.out && typeof j.out === "string") {
          const m = j.out.match(/->\s*([0-9]+)/);
          if (m) spawned = Number(m[1]);
        }
        if (spawned) addLog(`Spawned PID: ${spawned}`);
        else addLog(`No spawned PID parsed; check server logs /tmp/restore.out`);
        // refresh lists and remove saved
        await refreshAll();
      }
    } catch (err) {
      addLog(`RESTORE network error: ${err.message}`);
      console.error(err);
    } finally {
      setActionLoadingPid(null);
    }
  }

  // Remove saved entry manually (like "release" / forget)
  async function forgetSaved(oldpid) {
    if (!window.confirm(`Forget saved PID ${oldpid}?`)) return;
    // just locally remove; server memory will be eventually freed after restore; if you want server-side forget add endpoint.
    setSaved((s) => s.filter((x) => x.oldpid !== oldpid));
    addLog(`Forgot saved ${oldpid}`);
  }

  // yeh filter according to pid
  const filteredProcs = procs.filter((p) => {
    if (!searchPid.trim()) return true;
    return p.pid.toString().includes(searchPid.trim());
  });

  return (
    <div className="min-h-screen bg-gradient-to-br from-slate-900 via-slate-800 to-slate-900 text-slate-100 p-8">
      <div className="max-w-[1600px] mx-auto">
       <div className="mb-8">
          <h1 className="mb-2 text-transparent bg-clip-text bg-gradient-to-r from-blue-400 to-purple-400">
            Snapshot Manager
          </h1>
          <p className="text-slate-400">Web GUI for Process Management</p>
        </div>

        {apiConnected !== null && (
          <div className={`mb-6 rounded-lg p-4 border transition-all duration-300 ${
            apiConnected 
              ? 'bg-green-500/10 border-green-500/30' 
              : 'bg-red-500/10 border-red-500/30'
          }`}>
            <div className="flex items-center gap-3">
              {apiConnected ? (
                <>
                  <div className="flex items-center justify-center w-10 h-10 bg-green-500/20 rounded-full">
                    <svg className="w-6 h-6 text-green-400" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 12l2 2 4-4m6 2a9 9 0 11-18 0 9 9 0 0118 0z" />
                    </svg>
                  </div>
                  <div className="flex-1">
                    <h3 className="text-green-400">API Connected</h3>
                    <p className="text-green-300/70 text-sm">Successfully connected to {API_BASE}</p>
                  </div>
                  <div className="flex items-center gap-2 bg-green-500/20 px-3 py-1 rounded-full">
                    <div className="w-2 h-2 bg-green-400 rounded-full animate-pulse"></div>
                    <span className="text-green-400 text-sm">Online</span>
                  </div>
                </>
              ) : (
                <>
                  <div className="flex items-center justify-center w-10 h-10 bg-red-500/20 rounded-full">
                    <svg className="w-6 h-6 text-red-400" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 8v4m0 4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
                    </svg>
                  </div>
                  <div className="flex-1">
                    <h3 className="text-red-400">API Disconnected</h3>
                    <p className="text-red-300/70 text-sm">Unable to connect to {API_BASE}</p>
                  </div>
                  <div className="flex items-center gap-2 bg-red-500/20 px-3 py-1 rounded-full">
                    <div className="w-2 h-2 bg-red-400 rounded-full"></div>
                    <span className="text-red-400 text-sm">Offline</span>
                  </div>
                </>
              )}
            </div>
          </div>
        )}
               <div className="bg-slate-800/50 backdrop-blur-sm border border-slate-700 rounded-lg p-4 mb-6 flex items-center justify-between flex-wrap gap-4">
          <div className="flex items-center gap-4">
            <button
              onClick={refreshAll}
              disabled={loading}
              className="px-4 py-2 bg-blue-600 hover:bg-blue-700 disabled:bg-slate-600 disabled:cursor-not-allowed rounded-lg transition-all duration-200 flex items-center gap-2 shadow-lg hover:shadow-blue-500/50"
            >
              <svg className={`w-4 h-4 ${loading ? 'animate-spin' : ''}`} fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15" />
              </svg>
              {loading ? "Refreshing..." : "Refresh"}
            </button>
            
            <div className="flex items-center gap-6">
              <div className="flex items-center gap-2">
                <div className="w-2 h-2 bg-green-500 rounded-full animate-pulse"></div>
                <span className="text-slate-300">Processes: <span className="text-green-400">{procs.length}</span></span>
              </div>
              <div className="flex items-center gap-2">
                <div className="w-2 h-2 bg-purple-500 rounded-full"></div>
                <span className="text-slate-300">Saved: <span className="text-purple-400">{saved.length}</span></span>
              </div>
            </div>
          </div>

          
          <div className="relative">
            <svg className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-slate-400" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z" />
            </svg>
            <input
              type="text"
              placeholder="Search by PID..."
              value={searchPid}
              onChange={(e) => setSearchPid(e.target.value)}
              className="pl-10 pr-4 py-2 bg-slate-700/50 border border-slate-600 rounded-lg focus:outline-none focus:ring-2 focus:ring-blue-500 focus:border-transparent text-slate-100 placeholder-slate-400 w-64 transition-all duration-200"
            />
            {searchPid && (
              <button
                onClick={() => setSearchPid("")}
                className="absolute right-3 top-1/2 -translate-y-1/2 text-slate-400 hover:text-slate-200"
              >
                <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
                </svg>
              </button>
            )}
          </div>
        </div>

      
        <div className="grid grid-cols-1 xl:grid-cols-3 gap-6 mb-6">
         
          <div className="xl:col-span-2">
            <div className="bg-slate-800/50 backdrop-blur-sm border border-slate-700 rounded-lg overflow-hidden shadow-xl">
              <div className="bg-gradient-to-r from-blue-600 to-blue-700 px-6 py-4">
                <h2 className="text-slate-100">Running Processes</h2>
                {searchPid && (
                  <p className="text-blue-100 text-sm mt-1">
                    Showing {filteredProcs.length} of {procs.length} processes
                  </p>
                )}
              </div>
              
              <div className="overflow-x-auto">
                <table className="w-full">
                  <thead className="bg-slate-700/50">
                    <tr>
                      <th className="px-6 py-3 text-left text-slate-300">PID</th>
                      <th className="px-6 py-3 text-left text-slate-300">Name</th>
                      <th className="px-6 py-3 text-left text-slate-300">TTY</th>
                      <th className="px-6 py-3 text-left text-slate-300">GUI</th>
                      <th className="px-6 py-3 text-left text-slate-300">Actions</th>
                    </tr>
                  </thead>
                  <tbody className="divide-y divide-slate-700/50">
                    {filteredProcs.map((p) => {
                      const busy = actionLoadingPid === p.pid;
                      const saved = isSaved(p.pid);
                      return (
                        <tr key={p.pid} className="hover:bg-slate-700/30 transition-colors">
                          <td className="px-6 py-4">
                            <span className="inline-flex items-center justify-center px-2 py-1 bg-slate-700 rounded text-blue-400 font-mono">
                              {p.pid}
                            </span>
                          </td>
                          <td className="px-6 py-4 text-slate-200">{p.name}</td>
                          <td className="px-6 py-4 text-slate-400 font-mono">{p.tty || "—"}</td>
                          <td className="px-6 py-4">
                            {p.is_gui ? (
                              <span className="inline-flex items-center px-2 py-1 rounded-full bg-green-500/20 text-green-400 text-sm">
                                <svg className="w-3 h-3 mr-1" fill="currentColor" viewBox="0 0 20 20">
                                  <path fillRule="evenodd" d="M16.707 5.293a1 1 0 010 1.414l-8 8a1 1 0 01-1.414 0l-4-4a1 1 0 011.414-1.414L8 12.586l7.293-7.293a1 1 0 011.414 0z" clipRule="evenodd" />
                                </svg>
                                Yes
                              </span>
                            ) : (
                              <span className="inline-flex items-center px-2 py-1 rounded-full bg-slate-600/50 text-slate-400 text-sm">
                                No
                              </span>
                            )}
                          </td>
                          <td className="px-6 py-4">
                            <button
                              onClick={() => snapshot(p.pid)}
                              disabled={busy || saved}
                              title={saved ? "Already saved" : ""}
                              className={`px-4 py-2 rounded-lg transition-all duration-200 ${
                                saved
                                  ? "bg-slate-600 text-slate-400 cursor-not-allowed"
                                  : busy
                                  ? "bg-slate-600 text-slate-300 cursor-wait"
                                  : "bg-gradient-to-r from-orange-600 to-red-600 hover:from-orange-700 hover:to-red-700 text-white shadow-lg hover:shadow-red-500/50"
                              }`}
                            >
                              {busy ? (
                                <span className="flex items-center gap-2">
                                  <svg className="animate-spin w-4 h-4" fill="none" viewBox="0 0 24 24">
                                    <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4"></circle>
                                    <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path>
                                  </svg>
                                  Working...
                                </span>
                              ) : saved ? (
                                "Saved"
                              ) : (
                                "Snapshot & Kill"
                              )}
                            </button>
                          </td>
                        </tr>
                      );
                    })}
                    {filteredProcs.length === 0 && (
                      <tr>
                        <td colSpan="5" className="px-6 py-12 text-center text-slate-400">
                          <svg className="w-16 h-16 mx-auto mb-4 text-slate-600" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M9.172 16.172a4 4 0 015.656 0M9 10h.01M15 10h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
                          </svg>
                          {searchPid ? `No processes found matching PID "${searchPid}"` : "No processes found"}
                        </td>
                      </tr>
                    )}
                  </tbody>
                </table>
              </div>
            </div>
          </div>

         
          <div className="xl:col-span-1">
            <div className="bg-slate-800/50 backdrop-blur-sm border border-slate-700 rounded-lg overflow-hidden shadow-xl">
              <div className="bg-gradient-to-r from-purple-600 to-purple-700 px-6 py-4">
                <h2 className="text-slate-100">Saved Processes</h2>
                <p className="text-purple-100 text-sm mt-1">Restorable snapshots</p>
              </div>
              
              <div className="p-4">
                {saved.length === 0 ? (
                  <div className="text-center py-12">
                    <svg className="w-16 h-16 mx-auto mb-4 text-slate-600" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M5 8h14M5 8a2 2 0 110-4h14a2 2 0 110 4M5 8v10a2 2 0 002 2h10a2 2 0 002-2V8m-9 4h4" />
                    </svg>
                    <p className="text-slate-400">No saved snapshots yet</p>
                  </div>
                ) : (
                  <div className="space-y-3">
                    {saved.map((s) => {
                      const busy = actionLoadingPid === s.oldpid;
                      return (
                        <div key={s.oldpid} className="bg-slate-700/30 border border-slate-600 rounded-lg p-4 hover:border-purple-500/50 transition-all duration-200">
                          <div className="flex items-start justify-between mb-3">
                            <div className="flex-1">
                              <div className="flex items-center gap-2 mb-1">
                                <span className="inline-flex items-center px-2 py-1 bg-purple-500/20 text-purple-400 rounded text-sm font-mono">
                                  PID {s.oldpid}
                                </span>
                              </div>
                              <p className="text-slate-300 truncate">{s.name}</p>
                            </div>
                          </div>
                          <div className="flex gap-2">
                            <button
                              onClick={() => restore(s.oldpid)}
                              disabled={busy}
                              className={`flex-1 px-3 py-2 rounded-lg transition-all duration-200 ${
                                busy
                                  ? "bg-slate-600 text-slate-300 cursor-wait"
                                  : "bg-gradient-to-r from-green-600 to-emerald-600 hover:from-green-700 hover:to-emerald-700 text-white shadow-lg hover:shadow-green-500/50"
                              }`}
                            >
                              {busy ? (
                                <span className="flex items-center justify-center gap-2">
                                  <svg className="animate-spin w-4 h-4" fill="none" viewBox="0 0 24 24">
                                    <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4"></circle>
                                    <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4zm2 5.291A7.962 7.962 0 014 12H0c0 3.042 1.135 5.824 3 7.938l3-2.647z"></path>
                                  </svg>
                                  Working...
                                </span>
                              ) : (
                                "Restore"
                              )}
                            </button>
                            <button
                              onClick={() => forgetSaved(s.oldpid)}
                              className="px-3 py-2 bg-slate-600 hover:bg-red-600 text-slate-200 hover:text-white rounded-lg transition-all duration-200"
                              title="Forget this snapshot"
                            >
                              <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
                              </svg>
                            </button>
                          </div>
                        </div>
                      );
                    })}
                  </div>
                )}
              </div>

              <div className="border-t border-slate-700 bg-slate-800/30 p-4">
                <h3 className="text-slate-300 mb-2">Quick Diagnostics</h3>
                <p className="text-slate-400 text-sm mb-2">
                  After restore, check server logs:
                </p>
                <ul className="text-slate-400 text-sm space-y-1 font-mono">
                  <li className="flex items-center gap-2">
                    <span className="text-purple-400">→</span>
                    <span>/tmp/restore.out</span>
                  </li>
                  <li className="flex items-center gap-2">
                    <span className="text-purple-400">→</span>
                    <span>/tmp/snapshot_exec_err</span>
                  </li>
                  <li className="flex items-center gap-2">
                    <span className="text-purple-400">→</span>
                    <span>/tmp/snapshot_attach_log</span>
                  </li>
                </ul>
              </div>
            </div>
          </div>
        </div>

      
        <div className="bg-slate-800/50 backdrop-blur-sm border border-slate-700 rounded-lg overflow-hidden shadow-xl">
          <div className="bg-gradient-to-r from-slate-700 to-slate-600 px-6 py-4 flex items-center justify-between">
            <div className="flex items-center gap-2">
              <svg className="w-5 h-5 text-slate-300" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z" />
              </svg>
              <h2 className="text-slate-100">Activity Log</h2>
            </div>
            <span className="text-slate-400 text-sm">{log.length} entries</span>
          </div>
          
          <div className="p-4">
            <div className="bg-black/50 rounded-lg p-4 max-h-80 overflow-y-auto font-mono text-sm">
              {log.length === 0 ? (
                <div className="text-slate-500 text-center py-8">No activity yet</div>
              ) : (
                <div className="space-y-1">
                  {log.map((l, i) => (
                    <div key={i} className="text-green-400 hover:text-green-300 transition-colors">
                      {l}
                    </div>
                  ))}
                </div>
              )}
            </div>
          </div>
        </div>


        <div className="mt-6 bg-slate-800/30 border border-slate-700 rounded-lg p-4">
          <div className="flex items-start gap-3">
            <svg className="w-5 h-5 text-blue-400 mt-0.5 flex-shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
            </svg>
            <div className="text-slate-400 text-sm space-y-2">
              <p>
                <strong className="text-slate-300">Note:</strong> The server requires header <code className="px-2 py-0.5 bg-slate-700 rounded text-blue-400">x-snapshot-token</code>. 
                Make sure the Node server is running and has permission to access <code className="px-2 py-0.5 bg-slate-700 rounded text-blue-400">/dev/snapshotctl</code> (or run the server as root for development).
              </p>
              <p>
                The server must run helper ioctl, then kill the PID (snapshot), and on restore either spawn or reattach and return status text.
              </p>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}