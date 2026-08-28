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
//   curl -X POST -d "wifiState=bad_password" http://127.0.0.1:4173/mock/wifi
state.wifiState = 'connected';
// 'bad_password' with an ssid still set is a network that used to work;
// with the ssid cleared it is credentials that never did.
state.wifiProvisionFailure = 'unconfigured';
let scanPolls = null;
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
        wifiConnected: state.wifiState === 'connected' && Boolean(config.ssid),
        wifiState: config.ssid ? state.wifiState : 'unconfigured',
        wifiProvisionFailure: state.wifiProvisionFailure,
        wifiApActive: true,
        setupSsid: 'S2W-B9',
        stationIp: state.wifiState === 'connected' && config.ssid ? '10.0.88.184' : '',
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
        if (form.has('wifiState')) state.wifiState = form.get('wifiState');
        if (form.has('wifiProvisionFailure')) state.wifiProvisionFailure = form.get('wifiProvisionFailure');
        if (form.has('ssid')) config.ssid = form.get('ssid');
        return json(response, 200, { wifiState: state.wifiState });
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
            if (form.get('tcpListenPort') === '65535') {
                return json(response, 400, { error: 'invalid_port', field: 'tcpListenPort' });
            }
            if (form.get('baud') === '460800') {
                return json(response, 503, { error: 'serial_error', persisted: true });
            }
            config = { ...config, ...Object.fromEntries(form), wifiPasswordSaved: true };
            return json(response, 200, { ok: true });
        }
    }
    if (url.pathname === '/api/wifi/scan') {
        if (state.passwordSet && !state.authenticated) return json(response, 401, { error: 'authentication_required' });
        if (request.method === 'POST') {
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
