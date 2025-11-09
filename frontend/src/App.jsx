// frontend/src/App.jsx
import React, { useEffect, useState } from "react";

const API_BASE = "http://127.0.0.1:8000/api";
const TOKEN = "local-secret-change-me"; // change to a strong secret in prod

export default function App() {
  const [procs, setProcs] = useState([]);
  const [saved, setSaved] = useState([]); // saved snapshots (oldpid + metadata)
  const [loading, setLoading] = useState(false);
  const [actionLoadingPid, setActionLoadingPid] = useState(null);
  const [log, setLog] = useState([]);

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
      addLog(`Fetched ${j.procs?.length ?? 0} processes`);
    } catch (err) {
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

  return (
    <div style={{ padding: 20, fontFamily: "system-ui, Roboto, Arial" }}>
      <h2>Snapshot Manager (web GUI)</h2>

      <div style={{ marginBottom: 12 }}>
        <button onClick={refreshAll} disabled={loading}>
          {loading ? "Refreshing..." : "Refresh"}
        </button>
        <span style={{ marginLeft: 12, color: "#666" }}>Processes: {procs.length}</span>
        <span style={{ marginLeft: 12, color: "#666" }}>Saved: {saved.length}</span>
      </div>

      <div style={{ display: "flex", gap: 24 }}>
        <div style={{ flex: 1, minWidth: 420 }}>
          <h3>Running processes</h3>
          <div style={{ overflowX: "auto" }}>
            <table style={{ borderCollapse: "collapse", width: "100%" }} border="1" cellPadding="6">
              <thead style={{ background: "#f2f2f2" }}>
                <tr>
                  <th>PID</th>
                  <th>Name</th>
                  <th>TTY</th>
                  <th>GUI</th>
                  <th>Actions</th>
                </tr>
              </thead>
              <tbody>
                {procs.map((p) => {
                  const busy = actionLoadingPid === p.pid;
                  return (
                    <tr key={p.pid}>
                      <td>{p.pid}</td>
                      <td>{p.name}</td>
                      <td>{p.tty || "—"}</td>
                      <td>{p.is_gui ? "yes" : "no"}</td>
                      <td>
                        <button
                          onClick={() => snapshot(p.pid)}
                          disabled={busy || isSaved(p.pid)}
                          title={isSaved(p.pid) ? "Already saved" : ""}
                        >
                          {busy ? "Working..." : isSaved(p.pid) ? "Saved" : "Snapshot & Kill"}
                        </button>
                      </td>
                    </tr>
                  );
                })}
                {procs.length === 0 && (
                  <tr>
                    <td colSpan="5" style={{ textAlign: "center", padding: 20 }}>
                      No processes found
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </div>

        <div style={{ width: 420 }}>
          <h3>Saved processes (restorable)</h3>
          <div style={{ border: "1px solid #ddd", padding: 8, borderRadius: 6 }}>
            {saved.length === 0 ? (
              <div style={{ color: "#888", padding: 8 }}>No saved snapshots yet.</div>
            ) : (
              <table style={{ width: "100%" }} border="0" cellPadding="6">
                <thead>
                  <tr style={{ borderBottom: "1px solid #eee" }}>
                    <th>oldPID</th>
                    <th>Name</th>
                    <th style={{ textAlign: "right" }}>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {saved.map((s) => {
                    const busy = actionLoadingPid === s.oldpid;
                    return (
                      <tr key={s.oldpid}>
                        <td>{s.oldpid}</td>
                        <td style={{ maxWidth: 180, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
                          {s.name}
                        </td>
                        <td style={{ textAlign: "right" }}>
                          <button onClick={() => restore(s.oldpid)} disabled={busy}>
                            {busy ? "Working..." : "Restore"}
                          </button>
                          <button onClick={() => forgetSaved(s.oldpid)} style={{ marginLeft: 8 }}>
                            Forget
                          </button>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            )}
          </div>

          <h4 style={{ marginTop: 12 }}>Quick diagnostics</h4>
          <div style={{ color: "#666", fontSize: 13 }}>
            After restore, check server logs and these files on the host if the restored process is detached:
            <ul>
              <li>/tmp/restore.out</li>
              <li>/tmp/snapshot_exec_err</li>
              <li>/tmp/snapshot_attach_log</li>
            </ul>
          </div>
        </div>
      </div>

      <h3 style={{ marginTop: 20 }}>Activity log</h3>
      <div
        style={{
          maxHeight: 300,
          overflowY: "auto",
          background: "#0b0b0b",
          color: "#dfe",
          padding: 12,
          borderRadius: 6,
          fontFamily: "monospace",
        }}
      >
        {log.length === 0 ? <div style={{ color: "#888" }}>No activity yet.</div> : log.map((l, i) => <div key={i}>{l}</div>)}
      </div>

      <div style={{ marginTop: 12, color: "#666", fontSize: 13 }}>
        <div>
          Note: the server requires header <code>x-snapshot-token</code>. Make sure the Node server is running and has permission to
          access <code>/dev/snapshotctl</code> (or run the server as root for development).
        </div>
        <div style={{ marginTop: 6 }}>
          The server must: run helper ioctl, then kill the PID (snapshot), and on restore either spawn or reattach and return status text.
        </div>
      </div>
    </div>
  );
}
