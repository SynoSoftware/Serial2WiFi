(() => {
    const $ = (id) => document.getElementById(id);

    let csrfToken = '';
    let configurationAllowed = false;
    let scanTimer = null;
    let connectionAttemptActive = false;

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
            setButton($('scanAgain'), 'Scan', 'Scan for Wi-Fi networks');
            return;
        }

        setButton($('scanAgain'), 'Scan', 'Scan for Wi-Fi networks');

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
                    `${network.secured ? 'Secured' : 'Open'} · ${network.rssi} dBm`;

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
                    announce('Security token refreshed. Please save again.');
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

            setButton(button, 'Save', 'Connecting with saved settings');

            window.setTimeout(pollStatus, 500);
        } catch {
            announce('Save failed. Your entered values were preserved.');

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

    $('otherNetwork').addEventListener('click', showOtherNetwork);
    $('scanAgain').addEventListener('click', scanNetworks);
    $('wifiSecurity').addEventListener('change', setPasswordVisibility);

    $('showPassword').addEventListener('click', () => {
        setPasswordShown($('wifiPassword').type === 'password');
    });

    $('configuration').addEventListener('submit', save);

    window.setInterval(pollStatus, 2000);

    load();
})();
