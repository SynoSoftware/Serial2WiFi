(() => {
    const $ = (id) => document.getElementById(id);

    let csrfToken = '';
    let configurationAllowed = false;
    let scanTimer = null;
    let connectionAttemptActive = false;

    const terminalHistoryLimit = 32 * 1024;
    const terminalEncoder = new TextEncoder();
    const terminalDecoder = new TextDecoder();
    const terminalHistory = [];

    let terminalSocket = null;
    let terminalReconnectTimer = null;
    let terminalReconnectDelayMs = 1000;
    let terminalRenderPending = false;
    let terminalPaused = false;
    let terminalClosing = false;
    let terminalRxBytes = 0;
    let terminalTxBytes = 0;

    function terminalWebSocketUrl() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        return `${protocol}//${window.location.host}/terminal`;
    }

    function setTerminalConnection(state, text) {
        const connection = $('terminalConnection');
        if (!connection) {
            return;
        }

        const connected = state === 'connected';
        connection.dataset.state = state;
        connection.textContent = text;

        $('terminalSendInput').disabled = !connected;
        $('terminalSend').disabled = !connected;
    }

    function updateTerminalCounters() {
        const counters = $('terminalCounters');
        if (!counters) {
            return;
        }

        counters.textContent =
            `RX ${terminalRxBytes.toLocaleString()} B · ` +
            `TX ${terminalTxBytes.toLocaleString()} B`;
    }

    function rememberTerminalBytes(bytes) {
        for (const byte of bytes) {
            terminalHistory.push(byte);
        }

        const excess = terminalHistory.length - terminalHistoryLimit;
        if (excess > 0) {
            terminalHistory.splice(0, excess);
        }
    }

    function terminalHistoryBytes() {
        return Uint8Array.from(terminalHistory);
    }

    function terminalText(bytes) {
        const text = terminalDecoder.decode(bytes);
        let rendered = '';
        let carriageReturn = false;

        for (const character of text) {
            if (carriageReturn) {
                rendered += '\n';
                carriageReturn = false;

                if (character === '\n') {
                    continue;
                }
            }

            if (character === '\r') {
                carriageReturn = true;
                continue;
            }

            if (character === '\b') {
                if (rendered && !rendered.endsWith('\n')) {
                    rendered = rendered.slice(0, -1);
                }
                continue;
            }

            if (character === '\n' || character === '\t') {
                rendered += character;
                continue;
            }

            if (character.codePointAt(0) >= 0x20 && character !== '\u007f') {
                rendered += character;
            }
        }

        if (carriageReturn) {
            rendered += '\n';
        }

        return rendered;
    }

    function terminalHex(bytes) {
        const lines = [];

        for (let offset = 0; offset < bytes.length; offset += 16) {
            const line = bytes.subarray(offset, offset + 16);
            const hex = Array.from(line, (byte) =>
                byte.toString(16).padStart(2, '0').toUpperCase()
            ).join(' ');
            const ascii = Array.from(line, (byte) =>
                byte >= 0x20 && byte <= 0x7e
                    ? String.fromCharCode(byte)
                    : '.'
            ).join('');

            lines.push(`${hex.padEnd(47, ' ')}  ${ascii}`);
        }

        return lines.join('\n');
    }

    function renderTerminal() {
        const output = $('terminalOutput');
        if (!output || terminalPaused) {
            return;
        }

        const followOutput =
            output.scrollHeight - output.scrollTop - output.clientHeight < 32;
        const bytes = terminalHistoryBytes();
        const hex = $('terminalView').value === 'hex';

        output.classList.toggle('terminal-hex', hex);
        output.textContent = hex ? terminalHex(bytes) : terminalText(bytes);

        if (followOutput) {
            output.scrollTop = output.scrollHeight;
        }
    }

    function scheduleTerminalRender() {
        if (terminalPaused || terminalRenderPending) {
            return;
        }

        terminalRenderPending = true;
        window.requestAnimationFrame(() => {
            terminalRenderPending = false;
            renderTerminal();
        });
    }

    function receiveTerminalBytes(bytes) {
        terminalRxBytes += bytes.length;
        rememberTerminalBytes(bytes);
        updateTerminalCounters();
        scheduleTerminalRender();
    }

    function terminalLineEnding() {
        switch ($('terminalLineEnding').value) {
            case 'cr':
                return new Uint8Array([0x0d]);
            case 'lf':
                return new Uint8Array([0x0a]);
            default:
                return new Uint8Array([0x0d, 0x0a]);
        }
    }

    function concatenateBytes(first, second) {
        const bytes = new Uint8Array(first.length + second.length);
        bytes.set(first, 0);
        bytes.set(second, first.length);
        return bytes;
    }

    function sendTerminalBytes(bytes, localEcho = true) {
        if (!terminalSocket || terminalSocket.readyState !== WebSocket.OPEN) {
            return false;
        }

        terminalSocket.send(bytes);
        terminalTxBytes += bytes.length;

        if (localEcho && $('terminalLocalEcho').checked) {
            rememberTerminalBytes(bytes);
            scheduleTerminalRender();
        }

        updateTerminalCounters();
        return true;
    }

    function sendTerminalLine(event) {
        event.preventDefault();

        const input = $('terminalSendInput');
        const text = terminalEncoder.encode(input.value);
        const bytes = concatenateBytes(text, terminalLineEnding());

        if (sendTerminalBytes(bytes)) {
            input.value = '';
        }
    }

    function terminalControlByte(key) {
        if (key.length !== 1) {
            return null;
        }

        const code = key.toUpperCase().charCodeAt(0);
        return code >= 0x40 && code <= 0x5f ? code & 0x1f : null;
    }

    function sendTerminalKey(event) {
        if (event.isComposing || event.metaKey || event.altKey) {
            return;
        }

        if (event.ctrlKey) {
            if (event.key.toLowerCase() === 'v') {
                return;
            }

            if (
                event.key.toLowerCase() === 'c' &&
                window.getSelection()?.toString()
            ) {
                return;
            }

            const byte = terminalControlByte(event.key);
            if (byte !== null) {
                event.preventDefault();
                sendTerminalBytes(new Uint8Array([byte]), false);
            }
            return;
        }

        const specialKeys = {
            Enter: terminalLineEnding(),
            Backspace: new Uint8Array([0x08]),
            Delete: new Uint8Array([0x7f]),
            Tab: new Uint8Array([0x09]),
            Escape: new Uint8Array([0x1b]),
            ArrowUp: new Uint8Array([0x1b, 0x5b, 0x41]),
            ArrowDown: new Uint8Array([0x1b, 0x5b, 0x42]),
            ArrowRight: new Uint8Array([0x1b, 0x5b, 0x43]),
            ArrowLeft: new Uint8Array([0x1b, 0x5b, 0x44]),
            Home: new Uint8Array([0x1b, 0x5b, 0x48]),
            End: new Uint8Array([0x1b, 0x5b, 0x46])
        };

        if (specialKeys[event.key]) {
            event.preventDefault();

            const echo =
                event.key === 'Enter' ||
                event.key === 'Backspace' ||
                event.key === 'Tab';

            sendTerminalBytes(specialKeys[event.key], echo);
            return;
        }

        if (event.key.length === 1) {
            event.preventDefault();
            sendTerminalBytes(terminalEncoder.encode(event.key));
        }
    }

    function sendTerminalPaste(event) {
        const text = event.clipboardData?.getData('text');
        if (!text) {
            return;
        }

        event.preventDefault();
        sendTerminalBytes(terminalEncoder.encode(text));
    }

    function toggleTerminalPause() {
        terminalPaused = !terminalPaused;

        const button = $('terminalPause');
        button.textContent = terminalPaused ? 'Resume' : 'Pause';
        button.setAttribute('aria-pressed', String(terminalPaused));
        $('browserTerminal').dataset.paused = String(terminalPaused);

        if (!terminalPaused) {
            renderTerminal();
        }
    }

    function clearTerminal() {
        terminalHistory.length = 0;
        renderTerminal();
        $('terminalOutput').focus();
    }

    function scheduleTerminalReconnect() {
        if (terminalClosing || terminalReconnectTimer !== null) {
            return;
        }

        setTerminalConnection('connecting', 'Reconnecting…');

        terminalReconnectTimer = window.setTimeout(() => {
            terminalReconnectTimer = null;
            connectTerminal();
        }, terminalReconnectDelayMs);

        terminalReconnectDelayMs = Math.min(
            terminalReconnectDelayMs * 2,
            5000
        );
    }

    function connectTerminal() {
        if (
            terminalClosing ||
            terminalSocket?.readyState === WebSocket.OPEN ||
            terminalSocket?.readyState === WebSocket.CONNECTING
        ) {
            return;
        }

        setTerminalConnection('connecting', 'Connecting…');

        const socket = new WebSocket(terminalWebSocketUrl());
        socket.binaryType = 'arraybuffer';
        terminalSocket = socket;

        socket.addEventListener('open', () => {
            if (terminalSocket !== socket) {
                return;
            }

            terminalReconnectDelayMs = 1000;
            setTerminalConnection('connected', 'Connected');
        });

        socket.addEventListener('message', (event) => {
            if (
                terminalSocket !== socket ||
                !(event.data instanceof ArrayBuffer)
            ) {
                return;
            }

            receiveTerminalBytes(new Uint8Array(event.data));
        });

        socket.addEventListener('close', () => {
            if (terminalSocket !== socket) {
                return;
            }

            terminalSocket = null;
            setTerminalConnection('disconnected', 'Disconnected');
            scheduleTerminalReconnect();
        });

        socket.addEventListener('error', () => {
            if (terminalSocket === socket) {
                socket.close();
            }
        });
    }

    function stopTerminal() {
        terminalClosing = true;

        if (terminalReconnectTimer !== null) {
            window.clearTimeout(terminalReconnectTimer);
            terminalReconnectTimer = null;
        }

        terminalSocket?.close();
        terminalSocket = null;
    }

    function initializeTerminal() {
        if (!$('browserTerminal')) {
            return;
        }

        $('terminalView').addEventListener('change', renderTerminal);
        $('terminalPause').addEventListener('click', toggleTerminalPause);
        $('terminalClear').addEventListener('click', clearTerminal);
        $('terminalSendForm').addEventListener('submit', sendTerminalLine);
        $('terminalOutput').addEventListener('keydown', sendTerminalKey);
        $('terminalOutput').addEventListener('paste', sendTerminalPaste);

        setTerminalConnection('connecting', 'Connecting…');
        updateTerminalCounters();
        renderTerminal();
        connectTerminal();
    }

    function announce(message) {
        $('announcements').textContent = message;
    }

    function setButton(button, text, tooltip) {
        button.textContent = text;
        button.title = tooltip;
        button.setAttribute('aria-label', tooltip);
    }

    function setFieldError(field, message = '') {
        const input = $(field);
        const error = $(`${field}Error`);

        if (!error) {
            return;
        }

        error.textContent = message;
        error.hidden = !message;

        if (!input) {
            return;
        }

        input.setAttribute('aria-invalid', message ? 'true' : 'false');

        if (message) {
            input.setAttribute('aria-describedby', error.id);
        } else {
            input.removeAttribute('aria-describedby');
        }
    }

    function clearFieldErrors() {
        [
            'ssid',
            'wifiPassword',
            'wifiSecurity',
            'tcpHost',
            'tcpPort',
            'baud',
            'framing',
            'display'
        ].forEach((field) => setFieldError(field));
    }

    function setPasswordVisibility() {
        const secured = $('wifiSecurity').value === 'secured';

        $('passwordGroup').hidden = !secured;
        $('wifiPassword').disabled = !secured;
    }

    function setPasswordShown(shown) {
        $('wifiPassword').type = shown ? 'text' : 'password';
        $('showPassword').setAttribute('aria-pressed', String(shown));

        setButton(
            $('showPassword'),
            shown ? 'Hide' : 'Show',
            shown ? 'Hide Wi-Fi password' : 'Show Wi-Fi password'
        );
    }

    function clearNetworkSelection() {
        document.querySelectorAll('.network-row').forEach((row) => {
            row.setAttribute('aria-selected', 'false');
        });
    }

    function selectNetwork(ssid, secured, row) {
        $('ssid').value = ssid;
        $('wifiSecurity').value = secured ? 'secured' : 'open';
        $('manualSecurityGroup').hidden = true;

        clearNetworkSelection();
        row.setAttribute('aria-selected', 'true');

        setPasswordVisibility();

        if (secured) {
            $('wifiPassword').focus();
        }
    }

    function showOtherNetwork() {
        clearNetworkSelection();

        $('manualSecurityGroup').hidden = false;
        $('ssid').value = '';
        $('ssid').focus();

        setPasswordVisibility();
    }

    function renderScanState(state, networks = []) {
        const list = $('networkList');

        list.replaceChildren();

        if (state === 'scanning') {
            $('scanState').textContent = 'Scanning…';
            $('scanAgain').disabled = true;
            return;
        }

        $('scanAgain').disabled = false;

        if (state === 'failed') {
            $('scanState').textContent = 'Scan failed.';
            setButton(
                $('scanAgain'),
                'Scan',
                'Scan for Wi-Fi networks'
            );
            return;
        }

        setButton(
            $('scanAgain'),
            'Scan',
            'Scan for Wi-Fi networks'
        );

        if (networks.length === 0) {
            $('scanState').textContent = 'No networks found.';
            return;
        }

        $('scanState').textContent = '';

        networks.forEach((network) => {
            const row = document.createElement('button');

            row.type = 'button';
            row.className = 'network-row';
            row.setAttribute('role', 'option');
            row.setAttribute('aria-selected', 'false');

            const name = document.createElement('span');
            name.className = 'network-name';

            const meta = document.createElement('span');
            meta.className = 'network-meta';

            if (network.ssid) {
                name.textContent = network.ssid;
                meta.textContent =
                    `${network.secured ? 'Secured' : 'Open'} · ` +
                    `${network.rssi} dBm`;

                row.title = `Use ${network.ssid}`;

                row.addEventListener('click', () => {
                    selectNetwork(
                        network.ssid,
                        Boolean(network.secured),
                        row
                    );
                });
            } else {
                name.textContent = 'Hidden';
                meta.textContent = 'Enter network manually';
                row.title = 'Enter a hidden Wi-Fi network';

                row.addEventListener('click', showOtherNetwork);
            }

            row.append(name, meta);
            list.appendChild(row);
        });
    }

    function stopScanPolling() {
        if (scanTimer !== null) {
            window.clearTimeout(scanTimer);
            scanTimer = null;
        }
    }

    async function pollScan() {
        try {
            const response = await fetch('/api/wifi/scan');

            if (!response.ok) {
                throw new Error('Scan failed');
            }

            const result = await response.json();

            renderScanState(result.state, result.networks || []);

            if (result.state === 'scanning') {
                scanTimer = window.setTimeout(pollScan, 500);
            } else {
                scanTimer = null;
            }
        } catch {
            scanTimer = null;
            renderScanState('failed');
        }
    }

    async function refreshCsrfToken() {
        try {
            const response = await fetch('/api/config');

            if (!response.ok) {
                return false;
            }

            const config = await response.json();
            csrfToken = config.csrfToken || '';
            return csrfToken !== '';
        } catch {
            return false;
        }
    }

    async function scanNetworks() {
        stopScanPolling();
        renderScanState('scanning');

        try {
            const response = await fetch('/api/wifi/scan', {
                method: 'POST',
                headers: {
                    'X-CSRF-Token': csrfToken
                }
            });

            if (!response.ok) {
                if (response.status === 403) {
                    await refreshCsrfToken();
                }

                throw new Error('Scan failed');
            }

            await pollScan();
        } catch {
            renderScanState('failed');
        }
    }

    function fillConfig(config) {
        $('ssid').value = config.ssid || '';
        $('wifiSecurity').value =
            config.wifiSecurity === 'open' ? 'open' : 'secured';
        $('tcpHost').value = config.tcpHost || '';
        $('tcpPort').value = config.tcpPort || 0;
        $('baud').value = String(config.baud);
        $('framing').value = config.framing;
        $('display').value = config.display;

        $('manualSecurityGroup').hidden = Boolean(config.ssid);

        setPasswordVisibility();
    }

    function renderStatus(status) {
        $('wifiState').textContent =
            status.wifiConnected ? 'Connected' : 'Unavailable';

        $('tcpState').textContent =
            status.tcpState === 'connected'
                ? 'Connected'
                : status.tcpState;

        $('stationIp').textContent = status.stationIp || '—';
        $('statusBaud').textContent = status.baud || '—';
        $('statusFraming').textContent = status.framing || '—';

        const terminalSerialSettings = $('terminalSerialSettings');

        if (terminalSerialSettings) {
            terminalSerialSettings.textContent =
                `${status.baud || '—'} baud · ${status.framing || '—'}`;
        }

        $('statusS2N').textContent =
            `${status.serialToNetworkReceived || 0} bytes`;

        $('statusN2S').textContent =
            `${status.networkToSerialReceived || 0} bytes`;

        $('statusDrops').textContent =
            `${status.serialToNetworkDropped || 0} / ` +
            `${status.networkToSerialDropped || 0}`;
    }

    function renderConnectionAttempt(status) {
        if (!connectionAttemptActive) {
            return;
        }

        if (status.wifiConfigured && !status.wifiConnected) {
            announce('Wi-Fi unavailable. Retrying automatically…');
            return;
        }

        if (!status.wifiConfigured) {
            announce(
                status.tcpState === 'disabled'
                    ? 'Ready'
                    : 'Wi-Fi is not configured. TCP is waiting for Wi-Fi.'
            );

            if (status.tcpState === 'disabled') {
                finishConnectionAttempt();
            }

            return;
        }

        if (
            status.tcpState !== 'connected' &&
            status.tcpState !== 'disabled'
        ) {
            announce(
                status.tcpRetrying
                    ? 'Wi-Fi connected. Retrying automatically…'
                    : 'Wi-Fi connected. Connecting to server…'
            );

            return;
        }

        finishConnectionAttempt();
    }

    function finishConnectionAttempt() {
        connectionAttemptActive = false;

        $('save').disabled = false;
        setButton($('save'), 'Save', 'Save settings');
        announce('Ready');
    }

    async function pollStatus() {
        try {
            const response = await fetch('/api/status');

            if (!response.ok) {
                throw new Error('Status unavailable');
            }

            const status = await response.json();

            configurationAllowed = Boolean(status.configurationAllowed);

            if (status.setupSsid) {
                $('deviceName').textContent = status.setupSsid;
            }

            $('configuration').hidden = !configurationAllowed;

            renderStatus(status);
            renderConnectionAttempt(status);
        } catch {
            announce('Status unavailable.');
        }
    }

    async function load() {
        await pollStatus();

        if (!configurationAllowed) {
            return;
        }

        try {
            const response = await fetch('/api/config');

            if (!response.ok) {
                throw new Error('Configuration unavailable');
            }

            const config = await response.json();

            csrfToken = config.csrfToken || '';

            fillConfig(config);
            await scanNetworks();
        } catch {
            announce('Configuration is unavailable.');
        }
    }

    async function save(event) {
        event.preventDefault();

        clearFieldErrors();

        const button = $('save');

        button.disabled = true;
        setButton(button, 'Saving…', 'Saving settings');
        announce('Saving…');

        const values = new URLSearchParams({
            ssid: $('ssid').value,
            wifiSecurity: $('ssid').value
                ? $('wifiSecurity').value
                : 'unset',
            wifiPassword: $('wifiPassword').value,
            tcpHost: $('tcpHost').value,
            tcpPort: $('tcpPort').value || '0',
            baud: $('baud').value,
            framing: $('framing').value,
            display: $('display').value
        });

        try {
            const response = await fetch('/api/config', {
                method: 'POST',
                headers: {
                    'X-CSRF-Token': csrfToken,
                    'Content-Type':
                        'application/x-www-form-urlencoded'
                },
                body: values
            });

            const result = await response.json();

            if (!response.ok) {
                if (response.status === 403) {
                    await refreshCsrfToken();
                    announce(
                        'Security token refreshed. Please save again.'
                    );
                }

                if (result.field) {
                    setFieldError(
                        result.field,
                        result.error || 'Invalid value.'
                    );

                    $(result.field)?.focus();
                }

                announce(result.error || 'Save failed.');

                button.disabled = false;
                setButton(button, 'Save', 'Save settings');
                return;
            }

            connectionAttemptActive = true;

            announce(
                $('ssid').value
                    ? `Connecting to ${$('ssid').value}…`
                    : 'Applying settings…'
            );

            setButton(
                button,
                'Save',
                'Connecting with saved settings'
            );

            window.setTimeout(pollStatus, 500);
        } catch {
            announce(
                'Save failed. Your entered values were preserved.'
            );

            button.disabled = false;
            setButton(button, 'Save', 'Save settings');
        }
    }

    setButton(
        $('otherNetwork'),
        'Other',
        'Enter a Wi-Fi network manually'
    );

    setButton(
        $('scanAgain'),
        'Scan',
        'Scan for Wi-Fi networks'
    );

    setButton(
        $('save'),
        'Save',
        'Save settings'
    );

    setPasswordShown(false);

    $('otherNetwork').addEventListener(
        'click',
        showOtherNetwork
    );

    $('scanAgain').addEventListener(
        'click',
        scanNetworks
    );

    $('wifiSecurity').addEventListener(
        'change',
        setPasswordVisibility
    );

    $('showPassword').addEventListener('click', () => {
        setPasswordShown(
            $('wifiPassword').type === 'password'
        );
    });

    $('configuration').addEventListener('submit', save);

    initializeTerminal();

    window.addEventListener('pagehide', stopTerminal);
    window.setInterval(pollStatus, 2000);

    load();
})();