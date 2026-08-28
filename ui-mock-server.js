// Disposable UI verification harness. Serves data/ and mimics the firmware's
// HTTP contract (src/http_server.cpp) closely enough to drive every frontend
// journey: same error codes, same status codes, same field shapes.
//
// Drive scenarios from another terminal:
//   curl -X POST -d "authenticated=false" http://127.0.0.1:4173/mock/state
//   curl -X POST -d "passwordSet=false&terminalAvailable=true" http://127.0.0.1:4173/mock/state
//
// Save-outcome triggers (magic values, mirroring the firmware's responses):
//   tcpListenPort=65535 -> 400 {"error":"invalid_port","field":"tcpListenPort"}
//   baud=460800         -> 503 {"error":"serial_error","persisted":true}
//
// Trial outcomes are chosen before the attempt and delivered after a few polls,
// so the connecting panel is actually exercised:
//   curl -X POST -d "outcome=auth_failed&rssi=-84" http://127.0.0.1:4173/mock/wifi
//   curl -X POST -d "outcome=connected" http://127.0.0.1:4173/mock/wifi
//   curl -X POST -d "outcome=connected&polls=30" http://127.0.0.1:4173/mock/wifi
// Valid outcomes: connected, auth_failed, security_mismatch, not_found,
// could_not_connect, could_not_save.
const fs = require('fs');
const http = require('http');
const path = require('path');

const dataRoot = path.join(__dirname, 'data');
const state = {
    passwordSet: true,
    authenticated: false,
    terminalAvailable: true,
    authDown: false,
    configDown: false
};
// Not a boolean, so /mock/state cannot set it; assign it directly instead:
//   curl -X POST -d "wifiOutcome=auth_failed" http://127.0.0.1:4173/mock/wifi
state.wifiOutcome = 'connected';
// What the next trial will decide, and how many polls it takes to get there.
state.nextOutcome = 'connected';
state.nextRssi = 0;
state.nextPolls = 3;
let scanPolls = null;
// Mirrors wifi_access's trial state: the verdict is published only once the
// attempt is no longer running, and it outlives the page that started it.
let trial = { outcome: 'none', running: false, ssid: '', rssi: 0, polls: 0 };
let config = {
    ssid: 'Workshop',
    wifiSecurity: 'secured',
    wifiPasswordSaved: true,
    tcpMode: 'listen',
    tcpListenPort: 5000,
    tcpRemoteHost: '',
    tcpRemotePort: 0,
    baud: 19200,
    framing: '8N1',
    longPressMs: 500,
    longPressRepeatMs: 500,
    screenSaverSeconds: 30,
    csrfToken: 'mock-config-token'
};

function send(response, status, contentType, body) {
    response.writeHead(status, { 'Content-Type': contentType, 'Cache-Control': 'no-store' });
    response.end(body);
}

function json(response, status, value) {
    send(response, status, 'application/json', JSON.stringify(value));
}

function readForm(request) {
    return new Promise((resolve) => {
        let body = '';
        request.on('data', (chunk) => { body += chunk; });
        request.on('end', () => resolve(new URLSearchParams(body)));
    });
}

// The firmware reports its own compile-time stamp and reads the frontend one
// out of the filesystem image; here the process start stands in for the first
// and the real generated file supplies the second.
const firmwareBuild = stampNow();

function stampNow() {
    const now = new Date();
    const pad = (value) => String(value).padStart(2, '0');
    return `${pad(now.getFullYear() % 100)}${pad(now.getMonth() + 1)}${pad(now.getDate())}` +
        `-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`;
}

function frontendBuild() {
    try {
        return fs.readFileSync(path.join(dataRoot, 'build-stamp.txt'), 'utf8').trim();
    } catch {
        return '';
    }
}

function tcpState() {
    if (config.tcpMode === 'listen') {
        return Number(config.tcpListenPort) > 0 ? 'listening' : 'disabled';
    }
    return config.tcpRemoteHost && Number(config.tcpRemotePort) > 0
        ? 'connected'
        : 'disabled';
}

function status() {
    return {
        passwordSet: state.passwordSet,
        authenticated: state.authenticated,
        terminalAvailable: state.terminalAvailable,
        firmwareBuild,
        frontendBuild: frontendBuild(),
        wifiConfigured: Boolean(config.ssid),
        wifiConnected: state.wifiOutcome === 'connected' && Boolean(config.ssid),
        wifiOutcome: config.ssid ? state.wifiOutcome : 'none',
        wifiApActive: true,
        setupSsid: 'S2W-B9',
        stationIp: state.wifiOutcome === 'connected' && config.ssid ? '10.0.88.184' : '',
        // Derived from the saved record, like the firmware. Hardcoding these
        // made the runtime labels report a state no save could ever change.
        tcpMode: config.tcpMode,
        tcpState: tcpState(),
        tcpListenPort: Number(config.tcpListenPort),
        tcpRemoteHost: config.tcpRemoteHost,
        tcpRemotePort: Number(config.tcpRemotePort),
        baud: Number(config.baud),
        framing: config.framing,
        serialToNetworkReceived: 0,
        networkToSerialReceived: 0,
        serialToNetworkDropped: 0,
        networkToSerialDropped: 0
    };
}

http.createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1:4173');
    if (request.method === 'POST' && url.pathname === '/mock/wifi') {
        const form = await readForm(request);
        if (form.has('wifiOutcome')) state.wifiOutcome = form.get('wifiOutcome');
        if (form.has('outcome')) state.nextOutcome = form.get('outcome');
        if (form.has('rssi')) state.nextRssi = Number(form.get('rssi'));
        if (form.has('polls')) state.nextPolls = Number(form.get('polls'));
        if (form.has('ssid')) config.ssid = form.get('ssid');
        return json(response, 200, {
            wifiOutcome: state.wifiOutcome,
            nextOutcome: state.nextOutcome,
            nextRssi: state.nextRssi,
            nextPolls: state.nextPolls
        });
    }
    if (request.method === 'POST' && url.pathname === '/mock/state') {
        const form = await readForm(request);
        for (const key of Object.keys(state)) {
            if (form.has(key)) state[key] = form.get(key) === 'true';
        }
        return json(response, 200, state);
    }
    if (request.method === 'GET' && url.pathname === '/api/status') return json(response, 200, status());
    if (request.method === 'GET' && url.pathname === '/api/auth') {
        if (state.authDown) return json(response, 503, { error: 'unavailable' });
        return json(response, 200, {
            passwordSet: state.passwordSet,
            authenticated: state.authenticated,
            terminalAvailable: state.terminalAvailable,
            csrfToken: 'mock-auth-token'
        });
    }
    if (request.method === 'POST' && url.pathname === '/api/auth/login') {
        const form = await readForm(request);
        if (form.get('password') === 'wrong') return json(response, 401, { error: 'invalid_password' });
        state.authenticated = true;
        return json(response, 200, { ok: true, authenticated: true });
    }
    if (request.method === 'POST' && url.pathname === '/api/auth/logout') {
        state.authenticated = false;
        return json(response, 200, { ok: true, authenticated: false });
    }
    if (request.method === 'POST' && url.pathname === '/api/auth/password') {
        const form = await readForm(request);
        if (!state.passwordSet) {
            // Firmware bootstrap branch: AP-only, no session required, and
            // creating the password does NOT sign the browser in.
            if (!state.terminalAvailable) return json(response, 403, { error: 'bootstrap_requires_setup_ap' });
            if (!form.get('newPassword')) return json(response, 400, { error: 'password_required', field: 'newPassword' });
            state.passwordSet = true;
            state.authenticated = false;
            return json(response, 200, { ok: true });
        }
        if (!state.authenticated) return json(response, 401, { error: 'authentication_required' });
        if (form.get('currentPassword') === 'wrong') {
            return json(response, 403, { error: 'incorrect_password', field: 'currentPassword' });
        }
        // Firmware invalidates the session on password change.
        state.authenticated = false;
        return json(response, 200, { ok: true });
    }
    if (url.pathname === '/api/config') {
        if (!state.authenticated) return json(response, 401, { error: 'authentication_required' });
        if (state.configDown) return json(response, 500, { error: 'save_failed' });
        if (request.method === 'GET') return json(response, 200, config);
        if (request.method === 'POST') {
            const form = await readForm(request);
            // The firmware's Part 3 rule: only the stored network unchanged, or
            // the forget shape. Anything else is a credential write and belongs
            // to /api/wifi/trial.
            const ssid = form.get('ssid') || '';
            const security = form.get('wifiSecurity') || '';
            const password = form.get('wifiPassword') || '';
            const forget = ssid === '' && password === '' && security === 'unset';
            if (!forget) {
                const changed = password !== '' ? 'wifiPassword'
                    : ssid !== config.ssid ? 'ssid'
                    : security !== config.wifiSecurity ? 'wifiSecurity' : null;
                if (changed) {
                    return json(response, 400, {
                        error: 'credential_change_not_allowed', field: changed
                    });
                }
            }
            if (form.get('tcpListenPort') === '65535') {
                return json(response, 400, { error: 'invalid_port', field: 'tcpListenPort' });
            }
            if (form.get('baud') === '460800') {
                return json(response, 503, { error: 'serial_error', persisted: true });
            }
            config = { ...config, ...Object.fromEntries(form) };
            if (forget) {
                config.wifiPasswordSaved = false;
                state.wifiOutcome = 'none';
            }
            return json(response, 200, { ok: true });
        }
    }
    if (url.pathname === '/api/wifi/trial') {
        if (state.passwordSet && !state.authenticated) {
            return json(response, 401, { error: 'authentication_required' });
        }
        if (request.method === 'POST') {
            if (trial.running) return json(response, 409, { error: 'trial_running' });
            const form = await readForm(request);
            const ssid = form.get('ssid') || '';
            const password = form.get('wifiPassword') || '';
            if (!ssid) return json(response, 400, { error: 'network_required', field: 'ssid' });
            if (form.get('wifiSecurity') === 'secured') {
                if (password.length < 8 || password.length > 63) {
                    return json(response, 400, { error: 'invalid_length', field: 'wifiPassword' });
                }
                if (!/^[\x20-\x7e]+$/.test(password)) {
                    return json(response, 400, { error: 'invalid_characters', field: 'wifiPassword' });
                }
            } else if (password !== '') {
                return json(response, 400, { error: 'unexpected_password', field: 'wifiPassword' });
            }
            trial = {
                outcome: 'none',
                running: true,
                ssid,
                rssi: 0,
                polls: state.nextPolls,
                security: form.get('wifiSecurity'),
                password
            };
            return json(response, 202, { ok: true });
        }
        if (request.method === 'DELETE') {
            trial = { outcome: 'none', running: false, ssid: trial.ssid, rssi: 0, polls: 0 };
            return json(response, 200, { ok: true });
        }
        if (trial.running) {
            trial.polls -= 1;
            if (trial.polls <= 0) {
                trial.running = false;
                trial.outcome = state.nextOutcome;
                trial.rssi = state.nextRssi;
                if (trial.outcome === 'connected') {
                    // Like the firmware: the commit happens before Connected is
                    // published, so the saved record already names the network.
                    config.ssid = trial.ssid;
                    config.wifiSecurity = trial.security;
                    config.wifiPasswordSaved = trial.security === 'secured';
                    state.wifiOutcome = 'connected';
                }
            }
        }
        return json(response, 200, {
            outcome: trial.outcome,
            running: trial.running,
            ssid: trial.ssid,
            rssi: trial.rssi,
            ip: trial.outcome === 'connected' ? '10.0.88.184' : '',
            mdnsHost: 'serial2wifi-a1b4.local',
            apActive: state.terminalAvailable
        });
    }
    if (url.pathname === '/api/wifi/scan') {
        if (state.passwordSet && !state.authenticated) return json(response, 401, { error: 'authentication_required' });
        if (request.method === 'POST') {
            // Like the firmware: a scan would abort the attempt in flight and
            // produce a failure verdict for a password that was fine.
            if (trial.running) return json(response, 409, { error: 'trial_running' });
            scanPolls = 2;
            return json(response, 202, { ok: true });
        }
        // Like the firmware, report "scanning" for a couple of polls before
        // results arrive, so the frontend's polling loop is actually exercised.
        const scanState = scanPolls === null ? 'idle' : scanPolls > 0 ? 'scanning' : 'ready';
        if (scanPolls > 0) scanPolls -= 1;
        return json(response, 200, {
            state: scanState,
            networks: [
                { ssid: 'Workshop', secured: true, rssi: -48 },
                { ssid: 'Guest', secured: false, rssi: -62 },
                { ssid: 'Office', secured: true, rssi: -74 }
            ]
        });
    }

    const file = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
    const filePath = path.join(dataRoot, file);
    if (!filePath.startsWith(dataRoot) || !fs.existsSync(filePath)) return send(response, 404, 'text/plain', 'Not found');
    const contentType = file.endsWith('.css') ? 'text/css' : file.endsWith('.js') ? 'application/javascript' : 'text/html';
    send(response, 200, contentType, fs.readFileSync(filePath));
}).listen(4173, '127.0.0.1', () => console.log('UI mock listening on http://127.0.0.1:4173'));
