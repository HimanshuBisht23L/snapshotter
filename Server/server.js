// Server/server.js
// Robust Snapshot API: stores raw argv for restore, spawns with execve-style semantics.
// Usage: node Server/server.js

import express from "express";
import cors from "cors";
import { execFile, execSync, spawn as spawnChild } from "child_process";
import path from "path";
import fs from "fs";
import { promises as fsPromises } from "fs";

const PORT = 8000;
const HOST = "127.0.0.1";

// Absolute path to helper binary (ensure this is correct)
const HELPER_ABS = path.resolve(process.cwd(), "snapshot_user");

// If you add a sudoers rule for the helper, set this to true
const useSudo = false;

const app = express();
app.use(cors()); // dev: allow all origins; lock down in prod
app.use(express.json());

// tiny authentication: a shared secret header (change for production)
const SHARED_SECRET = process.env.SNAPSHOT_SECRET || "local-secret-change-me";
function requireAuth(req, res, next) {
  const token = req.header("x-snapshot-token");
  if (!token || token !== SHARED_SECRET) return res.status(401).json({ error: "unauthorized" });
  next();
}

/** runHelper: run helper binary and capture stdout/stderr */
function runHelper(args, timeout = 15000) {
  return new Promise((resolve, reject) => {
    const cmd = useSudo ? "sudo" : HELPER_ABS;
    const cmdArgs = useSudo ? [HELPER_ABS, ...args] : args;
    console.log(`[runHelper] ${cmd} ${cmdArgs.join(" ")}`);
    execFile(cmd, cmdArgs, { timeout }, (err, stdout, stderr) => {
      const out = stdout ? stdout.toString() : "";
      const errOut = stderr ? stderr.toString() : "";
      console.log("[runHelper] exit", err ? (err.code ?? err.message) : 0, "stdout=", out.trim(), "stderr=", errOut.trim());
      if (err) return reject({ err, stdout: out, stderr: errOut });
      resolve({ stdout: out, stderr: errOut });
    });
  });
}

/* in-memory saved metadata */
const savedList = []; // entries: { oldpid, cmdArgs (array|null), exe, tty, cwd, name, savedAt }

/* helper: read /proc/<pid>/cmdline as argv array (returns null on failure) */
async function readCmdlineArgs(pid) {
  try {
    const buf = await fsPromises.readFile(`/proc/${pid}/cmdline`);
    if (!buf || buf.length === 0) return null;
    const parts = buf.toString("utf8").split("\0").filter(Boolean);
    return parts.length ? parts : null;
  } catch (e) {
    return null;
  }
}

/* helper: read cwd symlink (best-effort) */
async function readCwd(pid) {
  try {
    const p = await fsPromises.readlink(`/proc/${pid}/cwd`);
    return p || "/";
  } catch (e) {
    return "/";
  }
}

/* helper: read exe path */
async function readExe(pid) {
  try {
    const p = await fsPromises.readlink(`/proc/${pid}/exe`);
    return p || "";
  } catch (e) {
    return "";
  }
}

/* helper: read controlling tty (fd0 or fd1) */
async function readTty(pid) {
  try {
    let p = "";
    try { p = await fsPromises.readlink(`/proc/${pid}/fd/0`); } catch(e) { p = ""; }
    if (!p) {
      try { p = await fsPromises.readlink(`/proc/${pid}/fd/1`); } catch(e) { p = ""; }
    }
    return p || "";
  } catch (e) {
    return "";
  }
}

/* health */
app.get("/api/health", (req, res) => {
  res.json({ ok: true, helper: HELPER_ABS, useSudo });
});

/* list processes (/proc scan) */
app.get("/api/processes", requireAuth, async (req, res) => {
  try {
    const procs = [];
    const d = await fsPromises.readdir("/proc", { withFileTypes: true });
    for (const de of d) {
      if (!/^\d+$/.test(de.name)) continue;
      const pid = Number(de.name);
      let comm = "";
      try { comm = (await fsPromises.readFile(`/proc/${pid}/comm`, "utf8")).trim(); } catch (e) {}
      let tty = "";
      try { tty = await fsPromises.readlink(`/proc/${pid}/fd/0`).catch(()=>""); } catch(e){}
      // best-effort is_gui detection: check /proc/<pid>/environ for DISPLAY= or WAYLAND_DISPLAY=
      let is_gui = false;
      try {
        const envbuf = await fsPromises.readFile(`/proc/${pid}/environ`, "utf8").catch(()=>"");
        if (envbuf && (envbuf.includes("DISPLAY=") || envbuf.includes("WAYLAND_DISPLAY="))) is_gui = true;
      } catch(e){}
      procs.push({ pid, name: comm || "", tty: tty || "", is_gui });
    }
    res.json({ procs });
  } catch (e) {
    console.error("process list error", e);
    res.status(500).json({ error: e.message });
  }
});

/* list saved snapshots */
app.get("/api/saved", requireAuth, async (req, res) => {
  try {
    // return a lightweight view to frontend
    const out = savedList.map(s => ({
      oldpid: s.oldpid,
      name: s.name,
      tty: s.tty,
      exe: s.exe,
      savedAt: s.savedAt
    }));
    res.json({ saved: out });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

/* snapshot endpoint: read metadata, call helper, then kill process and save metadata */
app.post("/api/snapshot", requireAuth, async (req, res) => {
  const pid = Number(req.body.pid);
  if (!Number.isInteger(pid) || pid <= 0) return res.status(400).json({ error: "invalid pid" });

  // capture metadata BEFORE killing
  let cmdArgs = null;
  let exe = "";
  let cwd = "/";
  let tty = "";
  try {
    cmdArgs = await readCmdlineArgs(pid);
    exe = await readExe(pid);
    cwd = await readCwd(pid);
    tty = await readTty(pid);
  } catch (e) {
    console.warn("metadata read failed:", e);
  }

  // name heuristic
  let name = (cmdArgs && cmdArgs.length) ? cmdArgs[0] : (exe ? exe.split("/").pop() : `pid:${pid}`);

  try {
    // call helper to snapshot at kernel level
    const { stdout } = await runHelper(["snapshot", String(pid)], 8000);

    // attempt to kill the process and its children (best-effort)
    let killErr = null;
    try {
      execSync(`pkill -TERM -P ${pid} || true; kill -9 ${pid} || true`, {
        stdio: "ignore",
        timeout: 3000,
      });
    } catch (err) {
      killErr = String(err && err.message ? err.message : err);
      console.error("kill step failed:", err);
    }

    // push metadata to saved list
    const entry = {
      oldpid: pid,
      cmdArgs: cmdArgs || null,
      exe: exe || "",
      tty: tty || "",
      cwd: cwd || "/",
      name: name || (`pid:${pid}`),
      savedAt: Date.now()
    };
    savedList.unshift(entry);

    return res.json({ ok: true, out: stdout.trim(), killErr, saved: { oldpid: entry.oldpid, name: entry.name, tty: entry.tty, exe: entry.exe } });
  } catch (e) {
    return res.status(500).json({ error: "snapshot failed", detail: e.stderr || e.err?.message || String(e) });
  }
});

/* restore endpoint: prefer to spawn using saved cmdArgs (execve-like), then call helper restore */
/* restore endpoint: prefer to spawn using saved cmdArgs (execve-like), then call helper restore */
app.post("/api/restore", requireAuth, async (req, res) => {
  const oldpid = Number(req.body.oldpid);
  let newpid = Number(req.body.newpid) || 0;
  if (!Number.isInteger(oldpid) || oldpid <= 0) return res.status(400).json({ error: "invalid oldpid" });

  const idx = savedList.findIndex(s => s.oldpid === oldpid);
  const meta = idx >= 0 ? savedList[idx] : null;

  // helper: safe which using execSync (no require(), we are ESM)
  const whichSync = (bin) => {
    try {
      // use shell to search PATH (returns empty string or path)
      const out = execSync(`command -v ${bin} 2>/dev/null || true`, { encoding: "utf8" }).trim();
      return out || null;
    } catch (e) {
      return null;
    }
  };

  if (meta) {
    // decide command and args
    let cmd = null;
    let args = [];
    if (meta.cmdArgs && meta.cmdArgs.length) {
      cmd = meta.cmdArgs[0];
      args = meta.cmdArgs.slice(1);
    } else if (meta.exe) {
      cmd = meta.exe;
      args = [];
    }

    // ==== spawn restored program preferably inside a terminal emulator ====
    if (cmd && (!newpid || newpid === 0)) {
      const termCandidates = [
        { bin: "terminator", argsBuilder: (c, a) => ["-x", c, ...a] },
        { bin: "gnome-terminal", argsBuilder: (c, a) => ["--", c, ...a] },
        { bin: "konsole", argsBuilder: (c, a) => ["-e", c, ...a] },
        // xfce4-terminal requires a single string for the -e form in many installs
        { bin: "xfce4-terminal", argsBuilder: (c, a) => ["-e", [c, ...a].map(x => typeof x === "string" ? x : String(x)).join(" ")] },
        { bin: "xterm", argsBuilder: (c, a) => ["-hold", "-e", c, ...a] }
      ];

      // prefer explicit env overrides, fallback to process.env
      const envDisplay = process.env.RESTORE_DISPLAY || process.env.DISPLAY || ":0";
      const envXauth = process.env.RESTORE_XAUTH || process.env.XAUTHORITY || (process.env.HOME ? `${process.env.HOME}/.Xauthority` : undefined);
      const restoreUser = process.env.RESTORE_USER || process.env.USER || null;

      // sanitize program + args
      const program = cmd;
      const programArgs = Array.isArray(args) ? args : [];

      // Builder for safe env for spawn
      const buildEnv = () => {
        const env = Object.assign({}, process.env);
        if (envDisplay) env.DISPLAY = envDisplay;
        if (envXauth) env.XAUTHORITY = envXauth;
        return env;
      };

      let spawnedPid = 0;
      let terminalLaunched = false;

      try {
        // 1) Try launching terminal directly as current server user
        for (const t of termCandidates) {
          const binPath = whichSync(t.bin);
          if (!binPath) continue;
          try {
            const termArgs = t.argsBuilder(program, programArgs);
            // If argsBuilder returned an array with a single combined string (e.g. xfce4-terminal), ensure type
            const spawnArgs = Array.isArray(termArgs) ? termArgs : [termArgs];

            const ch = spawnChild(binPath, spawnArgs, {
              detached: true,
              stdio: "ignore",
              cwd: meta.cwd || "/",
              env: buildEnv()
            });
            ch.unref();
            spawnedPid = ch.pid || 0;
            terminalLaunched = true;
            console.log("Launched terminal", binPath, "pid=", spawnedPid, "termArgs=", spawnArgs);
            break;
          } catch (e) {
            console.warn("Terminal spawn failed for", t.bin, e && e.message);
            continue;
          }
        }

        // 2) If direct launch failed and a restoreUser is configured, try sudo -u <user> <terminal ...>
        if (!terminalLaunched && restoreUser) {
          for (const t of termCandidates) {
            const binPath = whichSync(t.bin);
            if (!binPath) continue;
            try {
              const termArgs = t.argsBuilder(program, programArgs);
              const spawnArgs = Array.isArray(termArgs) ? termArgs : [termArgs];
              const sudoArgs = ["-u", restoreUser, "--", binPath, ...spawnArgs];
              const ch = spawnChild("sudo", sudoArgs, {
                detached: true,
                stdio: "ignore",
                cwd: meta.cwd || "/",
                env: buildEnv()
              });
              ch.unref();
              spawnedPid = ch.pid || 0;
              terminalLaunched = true;
              console.log("Launched terminal via sudo -u", restoreUser, "pid=", spawnedPid, "cmd=", ["sudo", ...sudoArgs].join(" "));
              break;
            } catch (e) {
              console.warn("sudo terminal spawn failed for", t.bin, e && e.message);
              continue;
            }
          }
        }

        // 3) Fallback: spawn headless with /tmp/restore.out (existing behaviour)
        if (!terminalLaunched) {
          try {
            const outPath = "/tmp/restore.out";
            const outfd = fs.openSync(outPath, "a");
            const ch = spawnChild(program, programArgs, {
              detached: true,
              stdio: ["ignore", outfd, outfd],
              cwd: meta.cwd || "/",
              env: buildEnv()
            });
            ch.unref();
            fs.closeSync(outfd);
            spawnedPid = ch.pid || 0;
            console.log("Fallback spawned headless PID:", spawnedPid);
          } catch (e) {
            console.error("Fallback headless spawn failed:", e && e.message);
          }
        }

        if (spawnedPid > 0) newpid = spawnedPid;
      } catch (e) {
        console.error("Restore spawn overall failed:", e && e.stack ? e.stack : e);
        // leave newpid===0 so helper will release the snapshot
      }
    }

  } else {
    // no server-side metadata
    if (!newpid || newpid === 0) {
      return res.status(400).json({ error: "no saved metadata for oldpid and newpid not provided" });
    }
  }

  // call helper restore ioctl with (oldpid, newpid)
  try {
    const { stdout } = await runHelper(["restore", String(oldpid), String(newpid)], 20000);
    if (idx >= 0) savedList.splice(idx, 1);
    return res.json({ ok: true, out: stdout.trim(), spawnedPid: newpid });
  } catch (e) {
    console.error("restore helper failed", e);
    return res.status(500).json({ error: "restore failed", detail: e.stderr || e.err?.message, stdout: e.stdout });
  }
});




/* logs endpoint: read helper logs & spawn logs */
app.get("/api/logs", requireAuth, async (req, res) => {
  try {
    const attach = await fsPromises.readFile("/tmp/snapshot_attach_log", "utf8").catch(()=>"");
    const spawn = await fsPromises.readFile("/tmp/snapshot_spawn_log", "utf8").catch(()=>"");
    const execerr = await fsPromises.readFile("/tmp/snapshot_exec_err", "utf8").catch(()=>"");
    const restoreOut = await fsPromises.readFile("/tmp/restore.out", "utf8").catch(()=>"");
    res.json({ attach, spawn, execerr, restoreOut });
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

app.listen(PORT, HOST, () => {
  console.log(`Snapshot API listening on http://${HOST}:${PORT}`);
  console.log("Use X-SNAPSHOT-TOKEN header with the shared secret to authenticate.");
});
