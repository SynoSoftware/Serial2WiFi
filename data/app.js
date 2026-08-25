(() => {
    const $ = (id) => document.getElementById(id);
    let csrfToken = '';
    let configurationAllowed = false;
    let scanTimer = 0;
    let connectionAttemptActive = false;

    function announce(message) { $('announcements').textContent = message; }

    function setFieldError(field, message) {
        const target = $(field + 'Error');
        if (target) target.textContent = message || '';
    }

    function setPasswordVisibility() {
        const secured = $('wifiSecurity').value === 'secured';
        $('passwordGroup').hidden = !secured;
        $('wifiPassword').disabled = !secured;
    }

    function selectNetwork(ssid, secured, row) {
        $('ssid').value = ssid;
        $('wifiSecurity').value = secured ? 'secured' : 'open';
        $('manualSecurityGroup').hidden = true;
        document.querySelectorAll('.network-row').forEach((item) => item.setAttribute('aria-selected', item === row ? 'true' : 'false'));
        setPasswordVisibility();
        if (secured) $('wifiPassword').focus();
    }

    function showOtherNetwork() {
        $('manualSecurityGroup').hidden = false;
        $('ssid').value = '';
        $('ssid').focus();
        setPasswordVisibility();
        document.querySelectorAll('.network-row').forEach((item) => item.setAttribute('aria-selected', 'false'));
    }

    function renderScanState(state, networks) {
        const list = $('networkList');
        list.innerHTML = '';
        if (state === 'scanning') {
            $('scanState').textContent = 'Scanning…';
            return;
        }
        if (state === 'failed') {
            $('scanState').textContent = 'Scan failed. Try again.';
            return;
        }
        if (!networks.length) {
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
            row.innerHTML = `<span class="network-name"></span><span class="network-meta"></span>`;
            if (network.ssid) {
                row.querySelector('.network-name').textContent = network.ssid;
                row.querySelector('.network-meta').textContent = `${network.secured ? 'Secured' : 'Open'} · ${network.rssi} dBm`;
                row.addEventListener('click', () => selectNetwork(network.ssid, network.secured, row));
            } else {
                row.querySelector('.network-name').textContent = 'Hidden network — enter manually';
                row.querySelector('.network-meta').textContent = 'Use Other network';
                row.addEventListener('click', showOtherNetwork);
            }
            list.appendChild(row);
        });
    }

    async function pollScan() {
        const response = await fetch('/api/wifi/scan');
        if (!response.ok) throw new Error('scan');
        const result = await response.json();
        renderScanState(result.state, result.networks || []);
        if (result.state === 'scanning') scanTimer = window.setTimeout(pollScan, 500);
    }

    async function scanNetworks() {
        renderScanState('scanning', []);
        try {
            const response = await fetch('/api/wifi/scan', { method: 'POST', headers: { 'X-CSRF-Token': csrfToken } });
            if (!response.ok) throw new Error('scan');
            await pollScan();
        } catch (_) {
            renderScanState('failed', []);
        }
    }

    function fillConfig(config) {
        $('ssid').value = config.ssid || '';
        $('wifiSecurity').value = config.wifiSecurity === 'open' ? 'open' : 'secured';
        $('tcpHost').value = config.tcpHost || '';
        $('tcpPort').value = config.tcpPort || 0;
        $('baud').value = String(config.baud);
        $('framing').value = config.framing;
        $('display').value = config.display;
        $('manualSecurityGroup').hidden = Boolean(config.ssid);
        setPasswordVisibility();
    }

    function renderStatus(status) {
        $('wifiState').textContent = status.wifiConnected ? 'Wi-Fi connected' : 'Wi-Fi unavailable';
        $('tcpState').textContent = status.tcpState === 'connected' ? 'TCP connected' : `TCP ${status.tcpState}`;
        $('stationIp').textContent = status.stationIp || '—';
        $('statusBaud').textContent = status.baud || '—';
        $('statusFraming').textContent = status.framing || '—';
        $('statusS2N').textContent = `${status.serialToNetworkReceived || 0} bytes`;
        $('statusN2S').textContent = `${status.networkToSerialReceived || 0} bytes`;
        $('statusDrops').textContent = `${status.serialToNetworkDropped || 0} / ${status.networkToSerialDropped || 0}`;
    }

    async function pollStatus() {
        try {
            const response = await fetch('/api/status');
            const status = await response.json();
            configurationAllowed = Boolean(status.configurationAllowed);
            if (status.setupSsid) $('deviceName').textContent = status.setupSsid;
            $('configuration').hidden = !configurationAllowed;
            renderStatus(status);
            if (connectionAttemptActive) {
                if (status.wifiConfigured && !status.wifiConnected) {
                    announce('Wi-Fi unavailable. Retrying automatically…');
                } else if (!status.wifiConfigured && status.tcpState === 'disabled') {
                    announce('Ready');
                    connectionAttemptActive = false;
                    $('save').disabled = false;
                    $('save').textContent = 'Save changes';
                } else if (!status.wifiConfigured) {
                    announce('Wi-Fi is not configured. TCP is waiting for Wi-Fi.');
                } else if (status.tcpState !== 'connected' && status.tcpState !== 'disabled') {
                    announce(status.tcpRetrying ?
                        'Wi-Fi connected. Retrying automatically…' :
                        'Wi-Fi connected. Connecting to server…');
                } else {
                    announce('Ready');
                    connectionAttemptActive = false;
                    $('save').disabled = false;
                    $('save').textContent = 'Save changes';
                }
            }
        } catch (_) {
            announce('Status unavailable.');
        }
    }

    async function load() {
        await pollStatus();
        if (!configurationAllowed) return;
        try {
            const response = await fetch('/api/config');
            const config = await response.json();
            csrfToken = config.csrfToken || '';
            fillConfig(config);
            await scanNetworks();
        } catch (_) {
            announce('Configuration is unavailable.');
        }
    }

    async function save(event) {
        event.preventDefault();
        ['ssid', 'wifiPassword', 'wifiSecurity', 'tcpHost', 'tcpPort', 'baud', 'framing', 'display']
            .forEach((field) => setFieldError(field, ''));
        const button = $('save');
        button.disabled = true;
        button.textContent = 'Saving…';
        announce('Saving…');
        const values = new URLSearchParams({
            ssid: $('ssid').value,
            wifiSecurity: $('ssid').value ? $('wifiSecurity').value : 'unset',
            wifiPassword: $('wifiPassword').value,
            tcpHost: $('tcpHost').value,
            tcpPort: $('tcpPort').value || '0',
            baud: $('baud').value,
            framing: $('framing').value,
            display: $('display').value
        });
        try {
            const response = await fetch('/api/config', { method: 'POST', headers: { 'X-CSRF-Token': csrfToken, 'Content-Type': 'application/x-www-form-urlencoded' }, body: values });
            const result = await response.json();
            if (!response.ok) {
                if (result.field) setFieldError(result.field, result.error || 'Invalid value.');
                if (result.field && $(result.field)) $(result.field).focus();
                announce(result.error || 'Save failed.');
            } else {
                connectionAttemptActive = true;
                announce(`Connecting to ${$('ssid').value || 'the configured network'}…`);
                button.textContent = 'Save changes';
                window.setTimeout(pollStatus, 500);
            }
        } catch (_) {
            announce('Save failed. Your entered values were preserved.');
        } finally {
            button.disabled = false;
            button.textContent = connectionAttemptActive ? 'Save changes' : 'Save and connect';
        }
    }

    $('otherNetwork').addEventListener('click', showOtherNetwork);
    $('scanAgain').addEventListener('click', scanNetworks);
    $('wifiSecurity').addEventListener('change', setPasswordVisibility);
    $('showPassword').addEventListener('click', () => {
        const input = $('wifiPassword');
        const shown = input.type === 'text';
        input.type = shown ? 'password' : 'text';
        $('showPassword').textContent = shown ? 'Show' : 'Hide';
        $('showPassword').setAttribute('aria-pressed', String(!shown));
    });
    $('scanState').addEventListener('click', () => { if ($('scanState').textContent.startsWith('Scan failed')) scanNetworks(); });
    $('configuration').addEventListener('submit', save);
    window.setInterval(pollStatus, 2000);
    load();
})();
