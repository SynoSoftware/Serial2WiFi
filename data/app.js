(() => {
    const $ = (id) => document.getElementById(id);

    const statusPollMs = 1000;
    const terminalHistoryLimit = 32 * 1024;
    const terminalMaxFrameBytes = 1024;
    const terminalEncoder = new TextEncoder();
    const terminalDecoder = new TextDecoder();
    const terminalHistory = [];

    let auth = {
        passwordSet: false,
        authenticated: false,
        terminalAvailable: false
    };
    let authAvailable = false;
    let authRetryTimer = null;
    let authCsrfToken = '';
    let configCsrfToken = '';
    let configurationBaseline = null;
    let wifiPasswordSaved = false;
    let configurationLoaded = false;
    let configurationLoading = false;
    let savePending = false;
    let lastStatus = null;
    let statusTimer = null;
    let statusSeen = false;
    let statusLost = false;
    let selectedView = null;

    let wifiMode = 'summary';
    let wifiSnapshot = null;
    let passwordEditing = false;
    let manualNetworkEdited = false;
    let scanTimer = null;

    let terminalWanted = false;
    let terminalSocket = null;
    let terminalReconnectTimer = null;
    let terminalReconnectDelayMs = 1000;
    let terminalRenderPending = false;
    let terminalPaused = false;
    let terminalClosing = false;
    let terminalRxBytes = 0;
    let terminalTxBytes = 0;

    function announce(message) {
        $('announcements').textContent = message;
    }

    function setIcon(use, name) {
        use.setAttribute('href', `#icon-${name}`);
    }

    function createIcon(name, className = '') {
        const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
        svg.classList.add('icon');
        if (className) {
            svg.classList.add(className);
        }
        svg.setAttribute('aria-hidden', 'true');
        const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
        use.setAttribute('href', `#icon-${name}`);
        svg.appendChild(use);
        return svg;
    }

    function safeJson(response) {
        return response
            .json()
            .catch(() => ({}));
    }

    function fieldNumber(id) {
        const value = $(id).value.trim();
        return value === '' ? 0 : Number(value);
    }

    function formatBytes(value) {
        const bytes = Number(value) || 0;
        if (bytes < 1024) {
            return `${bytes.toLocaleString()} B`;
        }
        if (bytes < 1024 * 1024) {
            return `${(bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0)} KB`;
        }
        return `${(bytes / (1024 * 1024)).toFixed(bytes < 10 * 1024 * 1024 ? 1 : 0)} MB`;
    }

    function endpoint(host, port) {
        if (!host || !Number(port)) {
            return '—';
        }
        return `${host}:${port}`;
    }

    function visibleTabs() {
        return Array.from(document.querySelectorAll('[role="tab"]')).filter(
            (tab) => !tab.hidden
        );
    }

    function selectView(view, focus = false) {
        const tabs = visibleTabs();
        const tab = tabs.find((candidate) => candidate.dataset.view === view);
        if (!tab) {
            return;
        }

        selectedView = view;
        document.body.classList.toggle('terminal-active', view === 'terminalView');
        document.querySelectorAll('[role="tab"]').forEach((candidate) => {
            const selected = candidate === tab;
            candidate.setAttribute('aria-selected', String(selected));
            candidate.tabIndex = selected ? 0 : -1;
        });
        ['setupView', 'statusView', 'terminalView'].forEach((id) => {
            $(id).hidden = id !== view;
        });

        if (view === 'setupView') {
            terminalWanted = false;
            stopTerminal();
            loadConfiguration();
        } else if (view === 'terminalView') {
            terminalWanted = true;
            connectTerminal();
            renderTerminal();
        } else {
            terminalWanted = false;
            stopTerminal();
        }

        if (focus) {
            tab.focus();
        }
    }

    function initializeTabs() {
        document.querySelectorAll('[role="tab"]').forEach((tab) => {
            tab.addEventListener('click', () => selectView(tab.dataset.view));
            tab.addEventListener('keydown', (event) => {
                const tabs = visibleTabs();
                const index = tabs.indexOf(tab);
                let next = null;

                if (event.key === 'ArrowRight') {
                    next = tabs[(index + 1) % tabs.length];
                } else if (event.key === 'ArrowLeft') {
                    next = tabs[(index - 1 + tabs.length) % tabs.length];
                } else if (event.key === 'Home') {
                    next = tabs[0];
                } else if (event.key === 'End') {
                    next = tabs[tabs.length - 1];
                }

                if (!next) {
                    return;
                }
                event.preventDefault();
                selectView(next.dataset.view, true);
            });
        });
    }

    function setFieldError(field, message = '') {
        const input = $(field);
        const error = $(`${field}Error`);
        if (!error) {
            return;
        }

        error.textContent = message;
        error.hidden = !message;
        if (input) {
            if (message) {
                input.setAttribute('aria-invalid', 'true');
            } else {
                input.removeAttribute('aria-invalid');
            }
        }
    }

    function tcpModeValue() {
        return document.querySelector('input[name="tcpMode"]:checked')?.value || 'listen';
    }

    function setTcpMode(mode) {
        const value = mode === 'connect' ? 'connect' : 'listen';
        document.querySelectorAll('input[name="tcpMode"]').forEach((input) => {
            input.checked = input.value === value;
        });
    }

    function focusField(field) {
        if (field === 'tcpMode') {
            document.querySelector('input[name="tcpMode"]:checked')?.focus();
            return;
        }
        $(field)?.focus();
    }

    function clearFieldErrors() {
        [
            'ssid',
            'wifiSecurity',
            'wifiPassword',
            'tcpMode',
            'tcpListenPort',
            'tcpRemoteHost',
            'tcpRemotePort',
            'baud',
            'framing',
            'longPressMs',
            'longPressRepeatMs',
            'screenSaverSeconds'
        ].forEach((field) => setFieldError(field));
    }

    function setAuthError(message = '') {
        $('authError').textContent = message;
        $('authError').hidden = !message;
    }

    function setPasswordChangeError(message = '') {
        $('passwordChangeError').textContent = message;
        $('passwordChangeError').hidden = !message;
    }

    function setPasswordChangeFeedback(message = '') {
        $('passwordChangeFeedback').textContent = message;
        $('passwordChangeFeedback').hidden = !message;
    }

    function setPasswordToggle(button, shown) {
        const input = $(button.dataset.passwordToggle);
        if (!input) return;
        input.type = shown ? 'text' : 'password';
        button.querySelector('span').textContent = shown ? 'Hide' : 'Show';
        setIcon(button.querySelector('use'), shown ? 'eye-off' : 'eye');
        button.setAttribute('aria-label', `${shown ? 'Hide' : 'Show'} password`);
    }

    function clearAdminPasswordFields() {
        ['currentAdminPassword', 'newAdminPassword', 'confirmAdminPassword'].forEach((id) => {
            $(id).value = '';
            $(id).type = 'password';
        });
        document.querySelectorAll('#passwordDialog [data-password-toggle]').forEach((button) => {
            setPasswordToggle(button, false);
        });
    }

    function closePasswordDialog() {
        clearAdminPasswordFields();
        setPasswordChangeError();
        setPasswordChangeFeedback();
        if ($('passwordDialog').open) {
            $('passwordDialog').close();
        }
    }

    async function fetchAuth() {
        const response = await fetch('/api/auth', { cache: 'no-store' });
        if (!response.ok) {
            throw new Error('Authentication state unavailable');
        }
        const state = await response.json();
        auth = {
            passwordSet: Boolean(state.passwordSet),
            authenticated: Boolean(state.authenticated),
            terminalAvailable: Boolean(state.terminalAvailable)
        };
        authAvailable = true;
        authCsrfToken = state.csrfToken || '';
        return auth;
    }

    function renderAuthPanel() {
        $('authLoginFields').hidden = true;
        $('authBootstrapFields').hidden = true;
        $('authPanel').removeAttribute('data-mode');
        setAuthError();

        if (!authAvailable) {
            $('authPanel').hidden = false;
            $('authPanel').dataset.mode = 'unavailable';
            $('authTitle').textContent = 'Management unavailable';
            $('authText').textContent =
                'Status is available, but sign-in is temporarily unavailable. Retrying automatically...';
            return;
        }

        if (auth.authenticated) {
            $('authPanel').hidden = true;
            return;
        }

        $('authPanel').hidden = false;
        if (!auth.passwordSet && auth.terminalAvailable) {
            $('authPanel').dataset.mode = 'bootstrap';
            $('authTitle').textContent = 'Secure this device';
            $('authText').textContent =
                'Create the administrator password to unlock device setup.';
            $('authBootstrapFields').hidden = false;
            return;
        }

        if (!auth.passwordSet) {
            $('authPanel').dataset.mode = 'info';
            $('authTitle').textContent = 'Administrator password not set';
            $('authText').textContent =
                'Connect through the Configuration AP to create the administrator password. Status remains available here.';
            return;
        }

        $('authPanel').dataset.mode = 'login';
        $('authTitle').textContent = 'Sign in to configure';
        $('authText').textContent = 'Status remains available without signing in.';
        $('authLoginFields').hidden = false;
    }

    function showPublicStatus() {
        selectedView = 'statusView';
        document.body.classList.remove('terminal-active');
        $('setupView').hidden = true;
        $('terminalView').hidden = true;
        $('statusView').hidden = false;
    }

    function applyAccessModel(preferSetup = false) {
        const authenticated = authAvailable && auth.authenticated;
        const firstUse = authAvailable && !auth.passwordSet && auth.terminalAvailable;

        $('bootView').hidden = true;
        $('navigation').hidden = !authenticated;
        $('setupTab').hidden = !authenticated;
        $('statusTab').hidden = !authenticated;
        $('terminalTab').hidden = !(authenticated && auth.terminalAvailable);
        $('securitySection').hidden = !authenticated;
        $('chooseNetwork').hidden = !auth.terminalAvailable;
        renderAuthPanel();

        if (!authenticated) {
            stopTerminal();
            document.body.classList.remove('terminal-active');
            if ($('passwordDialog').open) closePasswordDialog();
            if (firstUse) {
                selectedView = null;
                $('setupView').hidden = true;
                $('statusView').hidden = true;
                $('terminalView').hidden = true;
            } else {
                showPublicStatus();
            }
            return;
        }

        if (preferSetup || !selectedView || !visibleTabs().some((tab) => tab.dataset.view === selectedView)) {
            selectView('setupView');
        } else {
            selectView(selectedView);
        }
    }

    async function postAuth(path, values) {
        const body = new URLSearchParams(values).toString();
        const response = await fetch(path, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded',
                'X-CSRF-Token': authCsrfToken
            },
            body
        });
        return { response, result: await safeJson(response) };
    }

    async function loginWithPassword(password, preferSetup = true) {
        const { response, result } = await postAuth('/api/auth/login', { password });
        if (!response.ok) {
            throw new Error(result.error || 'Incorrect administrator password.');
        }
        await fetchAuth();
        if (!auth.authenticated) {
            throw new Error('Sign-in did not complete.');
        }
        applyAccessModel(preferSetup);
    }

    async function login() {
        const button = $('loginButton');
        const password = $('loginPassword').value;
        if (!password) {
            setAuthError('Enter the administrator password.');
            $('loginPassword').focus();
            return;
        }

        button.disabled = true;
        setAuthError();
        try {
            await loginWithPassword(password, true);
            $('loginPassword').value = '';
            announce('Signed in.');
        } catch (error) {
            setAuthError(error instanceof Error ? error.message : 'Sign-in failed.');
        } finally {
            button.disabled = false;
        }
    }

    async function bootstrapPassword() {
        const button = $('bootstrapButton');
        const password = $('bootstrapPassword').value;
        const confirm = $('bootstrapPasswordConfirm').value;

        if (!password) {
            setAuthError('Enter an administrator password.');
            $('bootstrapPassword').focus();
            return;
        }
        if (password !== confirm) {
            setAuthError('The passwords do not match.');
            $('bootstrapPasswordConfirm').focus();
            return;
        }

        button.disabled = true;
        setAuthError();
        try {
            const { response, result } = await postAuth('/api/auth/password', {
                newPassword: password
            });
            if (!response.ok) {
                throw new Error(result.error || 'Could not create the administrator password.');
            }

            await fetchAuth();
            if (!auth.authenticated) {
                await loginWithPassword(password, true);
            } else {
                applyAccessModel(true);
            }
            $('bootstrapPassword').value = '';
            $('bootstrapPasswordConfirm').value = '';
            announce('Administrator password created.');
        } catch (error) {
            setAuthError(error instanceof Error ? error.message : 'Could not create the administrator password.');
        } finally {
            button.disabled = false;
        }
    }

    async function logout() {
        try {
            await postAuth('/api/auth/logout', {});
            await fetchAuth();
        } catch {
            announce('Could not sign out.');
            return;
        }

        if (auth.authenticated) {
            announce('Could not sign out.');
            return;
        }

        configurationLoaded = false;
        configurationBaseline = null;
        configCsrfToken = '';
        $('configuration').hidden = true;
        $('saveRow').hidden = true;
        stopScanPolling();
        stopTerminal();
        applyAccessModel(false);
        announce('Signed out.');
    }

    async function updateAdminPassword() {
        const currentPassword = $('currentAdminPassword').value;
        const newPassword = $('newAdminPassword').value;
        const confirmPassword = $('confirmAdminPassword').value;
        const button = $('updatePassword');

        setPasswordChangeError();
        setPasswordChangeFeedback();
        if (!currentPassword) {
            setPasswordChangeError('Enter the current password.');
            $('currentAdminPassword').focus();
            return;
        }
        if (!newPassword) {
            setPasswordChangeError('Enter a new password.');
            $('newAdminPassword').focus();
            return;
        }
        if (newPassword !== confirmPassword) {
            setPasswordChangeError('The new passwords do not match.');
            $('confirmAdminPassword').focus();
            return;
        }

        button.disabled = true;
        try {
            const { response, result } = await postAuth('/api/auth/password', {
                currentPassword,
                newPassword
            });
            if (!response.ok) {
                throw new Error(result.error || 'Could not update the administrator password.');
            }

            closePasswordDialog();
            configurationLoaded = false;
            configurationBaseline = null;
            configCsrfToken = '';
            $('configuration').hidden = true;
            $('saveRow').hidden = true;
            stopScanPolling();
            stopTerminal();

            try {
                await fetchAuth();
            } catch {
                authAvailable = false;
                auth.authenticated = false;
                scheduleAuthRetry();
            }

            applyAccessModel(false);
            setAuthError('Password changed. Sign in again.');
            announce('Administrator password changed. Sign in again.');
        } catch (error) {
            setPasswordChangeError(error instanceof Error ? error.message : 'Could not update the administrator password.');
        } finally {
            button.disabled = false;
        }
    }

    function configurationValues() {
        const useSnapshot = wifiMode === 'manual' && !manualNetworkEdited && wifiSnapshot;
        const network = useSnapshot
            ? wifiSnapshot
            : {
                  ssid: $('ssid').value,
                  wifiSecurity: $('wifiSecurity').value,
                  wifiPassword: passwordEditing ? $('wifiPassword').value : ''
              };

        return {
            ssid: network.ssid,
            wifiSecurity: network.ssid ? network.wifiSecurity : 'unset',
            wifiPassword: network.wifiPassword || '',
            tcpMode: tcpModeValue(),
            tcpListenPort: String(fieldNumber('tcpListenPort')),
            tcpRemoteHost: $('tcpRemoteHost').value.trim(),
            tcpRemotePort: String(fieldNumber('tcpRemotePort')),
            baud: $('baud').value,
            framing: $('framing').value,
            longPressMs: String(fieldNumber('longPressMs')),
            longPressRepeatMs: String(fieldNumber('longPressRepeatMs')),
            screenSaverSeconds: String(fieldNumber('screenSaverSeconds'))
        };
    }

    function comparableValues(values) {
        return {
            ...values,
            wifiSecurity: values.ssid ? values.wifiSecurity : 'unset',
            wifiPassword: values.wifiPassword || '',
            tcpListenPort: String(Number(values.tcpListenPort) || 0),
            tcpRemotePort: String(Number(values.tcpRemotePort) || 0),
            longPressMs: String(Number(values.longPressMs) || 0),
            longPressRepeatMs: String(Number(values.longPressRepeatMs) || 0),
            screenSaverSeconds: String(Number(values.screenSaverSeconds) || 0)
        };
    }

    function sameConfiguration(first, second) {
        return first !== null && JSON.stringify(comparableValues(first)) === JSON.stringify(comparableValues(second));
    }

    function networkIdentityChanged(values = configurationValues()) {
        if (!configurationBaseline) {
            return false;
        }
        return values.ssid !== configurationBaseline.ssid || values.wifiSecurity !== configurationBaseline.wifiSecurity;
    }

    function networkConfigurationChanged(values = configurationValues()) {
        return networkIdentityChanged(values) || Boolean(values.wifiPassword);
    }


    function networkValues() {
        return {
            ssid: $('ssid').value,
            wifiSecurity: $('wifiSecurity').value,
            wifiPassword: passwordEditing ? $('wifiPassword').value : '',
            passwordEditing
        };
    }

    function savedNetworkValues() {
        const security = configurationBaseline?.wifiSecurity;
        return {
            ssid: configurationBaseline?.ssid || '',
            wifiSecurity: security === 'open' ? 'open' : 'secured',
            wifiPassword: '',
            passwordEditing: false
        };
    }

    function restoreNetwork(values) {
        $('ssid').value = values.ssid;
        $('wifiSecurity').value = values.wifiSecurity;
        $('wifiPassword').value = values.wifiPassword || '';
        passwordEditing = Boolean(values.passwordEditing);
    }

    function setWifiMode(mode) {
        wifiMode = mode;
        $('wifiSummary').hidden = mode !== 'summary';
        $('wifiChooser').hidden = mode !== 'chooser';
        $('manualNetworkFields').hidden = mode !== 'manual';
        renderPasswordEditor();
    }

    function renderWifiSummary() {
        const ssid = $('ssid').value;
        const secured = $('wifiSecurity').value === 'secured';
        const configured = Boolean(ssid);

        $('wifiConfigured').hidden = !configured;
        $('wifiEmpty').hidden = configured;
        $('wifiCurrentName').textContent = ssid || '—';
        $('wifiPending').hidden = !configured || !networkIdentityChanged();
        $('passwordSummary').hidden = !configured || !secured || passwordEditing;
        $('passwordState').textContent = wifiPasswordSaved ? 'Saved' : 'Not set';
        $('chooseNetwork').hidden = !auth.terminalAvailable;
        renderPasswordEditor();
    }

    function renderPasswordEditor() {
        const secured = $('wifiSecurity').value === 'secured';
        const visible = passwordEditing && secured;
        $('passwordEditor').hidden = !visible;
        $('wifiPassword').disabled = !visible;
        $('cancelPasswordChange').hidden =
            wifiMode !== 'summary' || networkIdentityChanged() || !wifiPasswordSaved;
    }

    function setPasswordShown(shown) {
        $('wifiPassword').type = shown ? 'text' : 'password';
        $('showPasswordText').textContent = shown ? 'Hide' : 'Show';
        setIcon($('showPasswordIcon'), shown ? 'eye-off' : 'eye');
    }

    function openNetworkChooser() {
        if (!auth.terminalAvailable) {
            showManualNetwork(true);
            return;
        }
        if (!wifiSnapshot) {
            wifiSnapshot = savedNetworkValues();
        }
        clearSaveFeedback();
        setWifiMode('chooser');
        scanNetworks();
    }

    function cancelNetworkEdit() {
        stopScanPolling();
        if (wifiSnapshot) {
            restoreNetwork(wifiSnapshot);
        }
        wifiSnapshot = null;
        manualNetworkEdited = false;
        setWifiMode('summary');
        renderWifiSummary();
        refreshSaveState();
    }

    function showManualNetwork(preserveCurrent = false) {
        stopScanPolling();
        if (!wifiSnapshot) {
            wifiSnapshot = savedNetworkValues();
        }
        if (!preserveCurrent) {
            $('ssid').value = '';
            $('wifiSecurity').value = 'secured';
            $('wifiPassword').value = '';
            passwordEditing = true;
        }
        manualNetworkEdited = false;
        clearSaveFeedback();
        setWifiMode('manual');
        renderPasswordEditor();
        refreshSaveState();
        $('ssid').focus();
    }

    function selectNetwork(ssid, secured) {
        const security = secured ? 'secured' : 'open';
        $('ssid').value = ssid;
        $('wifiSecurity').value = security;
        $('wifiPassword').value = '';

        const sameSavedNetwork =
            configurationBaseline &&
            configurationBaseline.ssid === ssid &&
            configurationBaseline.wifiSecurity === security;
        passwordEditing = secured && !(sameSavedNetwork && wifiPasswordSaved);
        manualNetworkEdited = false;
        setWifiMode('summary');
        renderWifiSummary();
        clearSaveFeedback();
        refreshSaveState();

        if (passwordEditing) {
            $('wifiPassword').focus();
        }
    }

    function changePassword() {
        $('wifiPassword').value = '';
        passwordEditing = true;
        clearSaveFeedback();
        setPasswordShown(false);
        renderWifiSummary();
        $('wifiPassword').focus();
    }

    function cancelPasswordChange() {
        $('wifiPassword').value = '';
        passwordEditing = false;
        clearSaveFeedback();
        setPasswordShown(false);
        renderWifiSummary();
        refreshSaveState();
    }

    function signalIcon(rssi) {
        if (rssi >= -55) return 'wifi-high';
        if (rssi >= -72) return 'wifi-medium';
        return 'wifi-low';
    }

    function signalText(rssi) {
        if (rssi >= -55) return 'Strong signal';
        if (rssi >= -72) return 'Good signal';
        if (rssi >= -82) return 'Fair signal';
        return 'Weak signal';
    }

    function normalizeNetworks(networks) {
        const best = new Map();
        networks.forEach((network) => {
            if (!network.ssid) return;
            const key = `${network.ssid}\u0000${Boolean(network.secured)}`;
            const existing = best.get(key);
            if (!existing || Number(network.rssi) > Number(existing.rssi)) {
                best.set(key, network);
            }
        });
        return Array.from(best.values()).sort((a, b) => Number(b.rssi) - Number(a.rssi));
    }

    function renderScanState(state, networks = []) {
        const list = $('networkList');
        list.replaceChildren();
        const pending = state === 'idle' || state === 'scanning';
        $('scanProgress').hidden = !pending;
        $('scanAgain').hidden = pending;

        if (state === 'idle') {
            $('scanState').textContent = 'Starting scan…';
            return;
        }
        if (state === 'scanning') {
            $('scanState').textContent = 'Scanning for networks…';
            return;
        }
        if (state === 'failed') {
            $('scanState').textContent = 'Could not scan for networks.';
            $('scanAgain').hidden = false;
            return;
        }

        const visible = normalizeNetworks(networks);
        $('scanAgain').hidden = false;
        if (visible.length === 0) {
            $('scanState').textContent = 'No networks found.';
            return;
        }
        $('scanState').textContent = 'Choose a network';

        visible.forEach((network) => {
            const row = document.createElement('button');
            row.type = 'button';
            row.className = 'network-row';
            row.setAttribute(
                'aria-label',
                `${network.ssid}, ${signalText(Number(network.rssi))}, ${network.secured ? 'secured' : 'open'}`
            );

            const leading = document.createElement('span');
            leading.className = 'network-leading';
            leading.appendChild(createIcon(signalIcon(Number(network.rssi))));
            const name = document.createElement('span');
            name.className = 'network-name';
            name.textContent = network.ssid;
            leading.appendChild(name);

            const meta = document.createElement('span');
            meta.className = 'network-meta';
            meta.textContent = signalText(Number(network.rssi)).replace(' signal', '');
            if (network.secured) {
                meta.appendChild(createIcon('lock'));
            }

            row.append(leading, meta);
            row.addEventListener('click', () => selectNetwork(network.ssid, Boolean(network.secured)));
            list.appendChild(row);
        });
    }

    function stopScanPolling() {
        if (scanTimer !== null) {
            window.clearTimeout(scanTimer);
            scanTimer = null;
        }
    }

    async function refreshConfigToken() {
        if (!auth.authenticated) return false;
        try {
            const response = await fetch('/api/config', { cache: 'no-store' });
            if (!response.ok) return false;
            const config = await response.json();
            configCsrfToken = config.csrfToken || '';
            return Boolean(configCsrfToken);
        } catch {
            return false;
        }
    }

    async function configFetch(url, init = {}, retry = true) {
        const response = await fetch(url, {
            ...init,
            headers: {
                ...(init.headers || {}),
                'X-CSRF-Token': configCsrfToken
            }
        });
        if (response.status === 403 && retry && (await refreshConfigToken())) {
            return configFetch(url, init, false);
        }
        return response;
    }

    async function pollScan(attempt = 0) {
        try {
            const response = await fetch('/api/wifi/scan', { cache: 'no-store' });
            if (!response.ok) throw new Error('scan failed');
            const result = await response.json();
            renderScanState(result.state, result.networks || []);
            if (result.state === 'idle' || result.state === 'scanning') {
                if (attempt >= 40) {
                    scanTimer = null;
                    renderScanState('failed');
                    return;
                }
                scanTimer = window.setTimeout(() => pollScan(attempt + 1), 500);
            } else {
                scanTimer = null;
            }
        } catch {
            scanTimer = null;
            renderScanState('failed');
        }
    }

    async function scanNetworks() {
        if (!auth.authenticated || !auth.terminalAvailable) {
            return;
        }
        stopScanPolling();
        renderScanState('scanning');
        try {
            const response = await configFetch('/api/wifi/scan', { method: 'POST' });
            if (!response.ok) throw new Error('scan failed');
            await pollScan();
        } catch {
            renderScanState('failed');
        }
    }

    function renderTcpFields() {
        const listen = tcpModeValue() === 'listen';
        $('tcpListenFields').hidden = !listen;
        $('tcpConnectFields').hidden = listen;
    }

    function updateScreenSaverPresentation() {
        const input = $('screenSaverSeconds');
        const off = input.value.trim() === '' || Number(input.value) === 0;
        input.closest('.unit-control')?.classList.toggle('unit-control-off', off);
    }

    function fillConfiguration(config) {
        const values = {
            ssid: config.ssid || '',
            wifiSecurity: config.wifiSecurity === 'open' ? 'open' : config.wifiSecurity === 'secured' ? 'secured' : 'unset',
            wifiPassword: '',
            tcpMode: config.tcpMode === 'connect' ? 'connect' : 'listen',
            tcpListenPort: String(Number(config.tcpListenPort) || 0),
            tcpRemoteHost: config.tcpRemoteHost || '',
            tcpRemotePort: String(Number(config.tcpRemotePort) || 0),
            baud: String(config.baud),
            framing: config.framing,
            longPressMs: String(config.longPressMs),
            longPressRepeatMs: String(config.longPressRepeatMs),
            screenSaverSeconds: String(Number(config.screenSaverSeconds) || 0)
        };

        $('ssid').value = values.ssid;
        $('wifiSecurity').value = values.wifiSecurity === 'unset' ? 'secured' : values.wifiSecurity;
        $('wifiPassword').value = '';
        setTcpMode(values.tcpMode);
        $('tcpListenPort').value = Number(values.tcpListenPort) ? values.tcpListenPort : '';
        $('tcpRemoteHost').value = values.tcpRemoteHost;
        $('tcpRemotePort').value = Number(values.tcpRemotePort) ? values.tcpRemotePort : '';
        $('baud').value = values.baud;
        $('framing').value = values.framing;
        $('longPressMs').value = values.longPressMs;
        $('longPressRepeatMs').value = values.longPressRepeatMs;
        $('screenSaverSeconds').value = Number(values.screenSaverSeconds) ? values.screenSaverSeconds : '';
        updateScreenSaverPresentation();

        wifiPasswordSaved = Boolean(config.wifiPasswordSaved);
        configCsrfToken = config.csrfToken || '';
        passwordEditing = false;
        wifiSnapshot = null;
        manualNetworkEdited = false;
        setPasswordShown(false);
        setWifiMode('summary');
        renderWifiSummary();
        renderTcpFields();

        configurationBaseline = { ...values };
        refreshSaveState();
    }

    function refreshSaveState() {
        if (!configurationLoaded || !configurationBaseline) {
            $('save').disabled = true;
            return;
        }
        const dirty = !sameConfiguration(configurationValues(), configurationBaseline);
        $('save').disabled = savePending || !auth.authenticated || !dirty;
        renderTcpRuntime(lastStatus);
    }

    function showSaveFeedback(message, state = 'success', icon = 'circle-check') {
        $('saveState').hidden = false;
        $('saveState').dataset.state = state;
        $('saveStateText').textContent = message;
        setIcon($('saveStateIcon'), icon);
    }

    function clearSaveFeedback() {
        $('saveState').hidden = true;
        $('saveStateText').textContent = '';
        $('saveState').removeAttribute('data-state');
    }

    function validateNumberField(id, min, max, allowZero = false) {
        const value = fieldNumber(id);
        if (!Number.isInteger(value) || (allowZero && value === 0 ? false : value < min) || (max !== null && value > max)) {
            setFieldError(id, max === null ? `Enter ${allowZero ? '0 or ' : ''}${min} or more.` : `Enter ${allowZero ? '0 or ' : ''}${min}–${max}.`);
            return false;
        }
        return true;
    }

    function validateConfiguration(values) {
        clearFieldErrors();
        let valid = true;

        if (values.wifiSecurity === 'secured') {
            const sameSavedNetwork =
                configurationBaseline &&
                values.ssid === configurationBaseline.ssid &&
                values.wifiSecurity === configurationBaseline.wifiSecurity &&
                wifiPasswordSaved;
            if (!sameSavedNetwork && !values.wifiPassword) {
                setFieldError('wifiPassword', 'Enter the password for this network.');
                valid = false;
            }
        }

        const listenPort = Number(values.tcpListenPort);
        const listenValid = Number.isInteger(listenPort) && listenPort >= 0 && listenPort <= 65535;
        if (!listenValid) {
            if (values.tcpMode === 'listen') {
                setFieldError('tcpListenPort', 'Enter a port from 0 to 65535.');
            } else {
                setFieldError('tcpMode', 'Saved Listen settings are invalid. Select Listen for client to correct them.');
            }
            valid = false;
        }

        const remotePort = Number(values.tcpRemotePort);
        const hasRemoteHost = Boolean(values.tcpRemoteHost);
        const hasRemotePort = Number.isInteger(remotePort) && remotePort > 0 && remotePort <= 65535;
        const remotePortInRange = Number.isInteger(remotePort) && remotePort >= 0 && remotePort <= 65535;
        const connectValid = hasRemoteHost === hasRemotePort && remotePortInRange;
        if (!connectValid) {
            if (values.tcpMode === 'connect') {
                if (!hasRemoteHost && remotePort !== 0) {
                    setFieldError('tcpRemoteHost', 'Enter the server name or address.');
                }
                if (hasRemoteHost && !hasRemotePort) {
                    setFieldError('tcpRemotePort', 'Enter a port from 1 to 65535.');
                } else if (!remotePortInRange) {
                    setFieldError('tcpRemotePort', 'Enter a port from 0 to 65535.');
                }
            } else {
                setFieldError('tcpMode', 'Saved Connect settings are invalid. Select Connect to server to correct them.');
            }
            valid = false;
        }

        valid = validateNumberField('longPressMs', 100, 1000) && valid;
        valid = validateNumberField('longPressRepeatMs', 250, 1000) && valid;
        const screenSaver = fieldNumber('screenSaverSeconds');
        if (!Number.isInteger(screenSaver) || screenSaver < 0 || (screenSaver > 0 && screenSaver < 5)) {
            setFieldError('screenSaverSeconds', 'Enter 5 seconds or more, or leave blank for Off.');
            valid = false;
        }

        if (!valid) {
            const firstInvalid = document.querySelector('[aria-invalid="true"]');
            if (firstInvalid?.id) focusField(firstInvalid.id);
        }
        return valid;
    }

    function adoptPersistedConfiguration(values) {
        const passwordWasSubmitted = Boolean(values.wifiPassword);
        const sameSecuredNetwork =
            configurationBaseline &&
            values.ssid === configurationBaseline.ssid &&
            values.wifiSecurity === 'secured' &&
            configurationBaseline.wifiSecurity === 'secured';

        wifiPasswordSaved =
            values.wifiSecurity === 'secured' && Boolean(values.ssid) &&
            (passwordWasSubmitted || (sameSecuredNetwork && wifiPasswordSaved));

        $('wifiPassword').value = '';
        passwordEditing = false;
        wifiSnapshot = null;
        manualNetworkEdited = false;
        setPasswordShown(false);
        setWifiMode('summary');
        renderWifiSummary();
        configurationBaseline = { ...values, wifiPassword: '' };
        refreshSaveState();
    }

    function backendErrorMessage(result, response) {
        if (result.error === 'invalid_timeout') {
            return 'Enter a value within the allowed range.';
        }
        if (response.status === 400 && result.field) {
            return 'Enter a valid value.';
        }
        if (response.status === 400) {
            return 'One or more settings are invalid.';
        }
        if (response.status === 500) {
            return 'Settings could not be stored.';
        }
        if (response.status === 403) {
            return 'This request is not allowed from the current interface.';
        }
        return 'Could not save settings.';
    }

    function showBackendFieldError(field, message) {
        setFieldError(field, message);
        const fieldMode = field === 'tcpListenPort'
            ? 'listen'
            : field === 'tcpRemoteHost' || field === 'tcpRemotePort'
              ? 'connect'
              : null;

        if (fieldMode && tcpModeValue() !== fieldMode) {
            setFieldError(
                'tcpMode',
                fieldMode === 'listen'
                    ? 'Saved Listen settings need attention. Select Listen for client to correct them.'
                    : 'Saved Connect settings need attention. Select Connect to server to correct them.'
            );
            focusField('tcpMode');
            return;
        }
        focusField(field);
    }

    async function handleSessionExpired() {
        try {
            await fetchAuth();
        } catch {
            authAvailable = false;
            auth.authenticated = false;
            scheduleAuthRetry();
        }
        applyAccessModel(false);
        if (authAvailable) {
            setAuthError('Your session expired. Sign in again; your entered settings are preserved.');
        }
    }

    async function saveConfiguration(event) {
        event.preventDefault();
        const values = configurationValues();
        if (!validateConfiguration(values)) {
            return;
        }

        savePending = true;
        let responseReceived = false;
        $('save').disabled = true;
        $('saveText').textContent = 'Saving…';
        showSaveFeedback('Saving changes…', 'warning', 'loader-circle');
        $('saveStateIcon').closest('svg').classList.add('spin');

        const body = new URLSearchParams({
            ssid: values.ssid,
            wifiSecurity: values.wifiSecurity,
            wifiPassword: values.wifiPassword,
            tcpMode: values.tcpMode,
            tcpListenPort: values.tcpListenPort,
            tcpRemoteHost: values.tcpRemoteHost,
            tcpRemotePort: values.tcpRemotePort,
            baud: values.baud,
            framing: values.framing,
            longPressMs: values.longPressMs,
            longPressRepeatMs: values.longPressRepeatMs,
            screenSaverSeconds: values.screenSaverSeconds
        }).toString();

        try {
            const response = await configFetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body
            });
            responseReceived = true;
            const result = await safeJson(response);

            if (response.status === 401) {
                await handleSessionExpired();
                return;
            }

            if (!response.ok) {
                if (result.error === 'serial_error' && result.persisted === true) {
                    adoptPersistedConfiguration(values);
                    showSaveFeedback(
                        'Settings saved, but the serial configuration could not be applied.',
                        'warning',
                        'triangle-alert'
                    );
                    announce('Settings saved, but the serial configuration could not be applied.');
                    return;
                }

                if (result.field && $(`${result.field}Error`)) {
                    showBackendFieldError(result.field, backendErrorMessage(result, response));
                }
                throw new Error(backendErrorMessage(result, response));
            }

            adoptPersistedConfiguration(values);
            showSaveFeedback('Changes saved.', 'success', 'circle-check');
            announce('Changes saved.');

            window.setTimeout(refreshStatusNow, 250);
        } catch (error) {
            if (!responseReceived) {
                showSaveFeedback(
                    'Could not confirm whether settings were saved. Your entries are preserved.',
                    'warning',
                    'triangle-alert'
                );
                announce('Could not confirm whether settings were saved. Your entered values are preserved.');
            } else {
                showSaveFeedback(error instanceof Error ? error.message : 'Could not save settings.', 'danger', 'circle-x');
                announce('Settings were not saved. Entered values were preserved.');
            }
        } finally {
            savePending = false;
            $('saveText').textContent = 'Save changes';
            $('saveStateIcon').closest('svg').classList.remove('spin');
            refreshSaveState();
        }
    }

    async function loadConfiguration() {
        if (!auth.authenticated || configurationLoaded || configurationLoading) {
            return;
        }
        configurationLoading = true;
        $('setupLoading').hidden = false;
        $('setupUnavailable').hidden = true;
        $('configuration').hidden = true;
        $('saveRow').hidden = true;

        try {
            const response = await fetch('/api/config', { cache: 'no-store' });
            if (response.status === 401) {
                await handleSessionExpired();
                return;
            }
            if (!response.ok) {
                throw new Error('Settings could not be loaded.');
            }
            const config = await response.json();
            fillConfiguration(config);
            configurationLoaded = true;
            refreshSaveState();
            $('setupLoading').hidden = true;
            $('configuration').hidden = false;
            $('saveRow').hidden = false;
        } catch (error) {
            $('setupLoading').hidden = true;
            $('setupUnavailable').hidden = false;
            $('saveRow').hidden = true;
            $('setupUnavailableText').textContent =
                error instanceof Error ? error.message : 'Settings could not be loaded.';
        } finally {
            configurationLoading = false;
        }
    }

    function tcpModeText(mode) {
        return mode === 'connect' ? 'Connect to server' : 'Listen for client';
    }

    function tcpStateText(state, mode) {
        switch (state) {
            case 'disabled': return 'Disabled';
            case 'waiting_for_wifi': return 'Waiting for Wi-Fi';
            case 'listening': return 'Listening';
            case 'connecting': return 'Connecting';
            case 'retrying': return 'Retrying';
            case 'connected': return mode === 'listen' ? 'Client connected' : 'Connected';
            case 'failure': return 'Failure';
            default: return '—';
        }
    }

    function tcpRuntime(status) {
        if (!status) return { text: '', state: 'neutral' };
        const mode = status.tcpMode === 'connect' ? 'connect' : 'listen';
        const state = status.tcpState;
        const remote = endpoint(status.tcpRemoteHost, status.tcpRemotePort);
        const listenPort = Number(status.tcpListenPort) || 0;
        const configured = mode === 'listen'
            ? listenPort > 0
            : Boolean(status.tcpRemoteHost) && Number(status.tcpRemotePort) > 0;

        switch (state) {
            case 'disabled':
                return configured
                    ? { text: 'Current · TCP unavailable', state: 'warning' }
                    : { text: 'Current · Not configured', state: 'neutral' };
            case 'waiting_for_wifi':
                return { text: 'Current · Waiting for Wi-Fi', state: 'warning' };
            case 'listening':
                return { text: `Current · Listening on port ${listenPort}`, state: 'success' };
            case 'connecting':
                return { text: `Current · Connecting to ${remote}…`, state: 'warning' };
            case 'retrying':
                return { text: `Current · ${remote} unavailable · retrying`, state: 'warning' };
            case 'connected':
                return mode === 'listen'
                    ? { text: 'Current · TCP client connected', state: 'success' }
                    : { text: `Current · Connected to ${remote}`, state: 'success' };
            case 'failure':
                return { text: 'Current · TCP failure', state: 'danger' };
            default:
                return { text: '', state: 'neutral' };
        }
    }

    function renderTcpRuntime(status) {
        if (!configurationLoaded || !status) {
            $('tcpRuntime').hidden = true;
            return;
        }
        const runtime = tcpRuntime(status);
        if (!runtime.text) {
            $('tcpRuntime').hidden = true;
            return;
        }
        $('tcpRuntime').hidden = false;
        $('tcpRuntime').textContent = runtime.text;
        $('tcpRuntime').dataset.state = runtime.state;
    }

    function renderBridgeState(status) {
        const mode = status.tcpMode === 'connect' ? 'connect' : 'listen';
        const remote = endpoint(status.tcpRemoteHost, status.tcpRemotePort);
        const listenPort = Number(status.tcpListenPort) || 0;
        const tcpConfigured = mode === 'listen'
            ? listenPort > 0
            : Boolean(status.tcpRemoteHost) && Number(status.tcpRemotePort) > 0;
        let state = 'neutral';
        let title = 'Not configured';
        let detail = '';

        if (!status.wifiConfigured || !tcpConfigured) {
            title = 'Not configured';
            if (!status.wifiConfigured && !tcpConfigured) {
                detail = 'Configure Wi-Fi and TCP before using the serial bridge.';
            } else if (!status.wifiConfigured) {
                detail = 'Configure Wi-Fi before using the serial bridge.';
            } else {
                detail = mode === 'listen'
                    ? 'Set a listening port before using the serial bridge.'
                    : 'Set a server and port before using the serial bridge.';
            }
        } else if (!status.wifiConnected) {
            title = 'Offline';
            state = 'warning';
            detail = 'Wi-Fi is not connected. TCP is waiting for Wi-Fi.';
        } else {
            switch (status.tcpState) {
                case 'disabled':
                    title = 'TCP unavailable';
                    detail = 'TCP is not running.';
                    break;
                case 'waiting_for_wifi':
                    title = 'Waiting for Wi-Fi';
                    state = 'warning';
                    detail = 'TCP will start when Wi-Fi is connected.';
                    break;
                case 'listening':
                    title = 'Ready';
                    state = 'success';
                    detail = `Listening on port ${listenPort.toLocaleString()} for a TCP client.`;
                    break;
                case 'connecting':
                    title = 'Connecting';
                    state = 'warning';
                    detail = `Connecting to ${remote}...`;
                    break;
                case 'retrying':
                    title = 'Retrying';
                    state = 'warning';
                    detail = `Could not reach ${remote}. Retrying automatically.`;
                    break;
                case 'connected':
                    title = 'Active';
                    state = 'success';
                    detail = mode === 'listen'
                        ? 'TCP client connected. Serial to TCP forwarding is active.'
                        : `Connected to ${remote}. Serial to TCP forwarding is active.`;
                    break;
                case 'failure':
                    title = 'TCP failure';
                    state = 'danger';
                    detail = mode === 'listen'
                        ? 'The TCP listener could not start.'
                        : `The connection to ${remote} failed.`;
                    break;
                default:
                    title = 'Unknown';
                    state = 'warning';
                    detail = 'TCP state is unavailable.';
            }
        }

        $('bridgeState').textContent = title;
        $('bridgeState').dataset.state = state;
        $('bridgeDetail').textContent = detail;
    }

    function renderStatus(status) {
        lastStatus = status;
        statusSeen = true;
        if (status.setupSsid) {
            $('deviceName').textContent = status.setupSsid;
        }

        $('statusWifi').textContent = status.wifiConnected
            ? 'Connected'
            : status.wifiConfigured
              ? 'Not connected'
              : 'Not configured';
        $('stationIp').textContent = status.stationIp && status.stationIp !== '0.0.0.0' ? status.stationIp : '—';
        $('configurationApState').textContent = status.wifiApActive ? 'On' : 'Off';

        const mode = status.tcpMode === 'connect' ? 'connect' : 'listen';
        const listenPort = Number(status.tcpListenPort) || 0;
        const remotePort = Number(status.tcpRemotePort) || 0;
        const tcpConfigured = mode === 'listen'
            ? listenPort > 0
            : Boolean(status.tcpRemoteHost) && remotePort > 0;
        $('statusTcpMode').textContent = tcpModeText(mode);
        $('statusTcpState').textContent = status.tcpState === 'disabled' && !tcpConfigured
            ? 'Not configured'
            : tcpStateText(status.tcpState, mode);
        if (mode === 'listen') {
            $('statusTcpEndpointLabel').textContent = 'Listening port';
            $('statusTcpEndpoint').textContent = listenPort ? listenPort.toLocaleString() : '—';
        } else {
            $('statusTcpEndpointLabel').textContent = 'Server';
            $('statusTcpEndpoint').textContent = endpoint(status.tcpRemoteHost, status.tcpRemotePort);
        }

        const baud = Number(status.baud) || 0;
        $('statusBaud').textContent = baud ? baud.toLocaleString() : '—';
        $('statusFraming').textContent = status.framing || '—';
        $('statusS2N').textContent = formatBytes(status.serialToNetworkReceived);
        $('statusN2S').textContent = formatBytes(status.networkToSerialReceived);
        $('statusDrops').textContent =
            `S\u2192T ${(Number(status.serialToNetworkDropped) || 0).toLocaleString()} \u00b7 ` +
            `T\u2192S ${(Number(status.networkToSerialDropped) || 0).toLocaleString()}`;

        $('terminalSerialSettings').textContent = `${baud ? baud.toLocaleString() : '—'} baud \u00b7 ${status.framing || '—'}`;
        renderBridgeState(status);
        renderTcpRuntime(status);
        updateTerminalWriteAccess();
    }

    function markStatusLost() {
        statusLost = true;
        $('globalNotice').hidden = false;
        $('globalNoticeText').textContent = 'Device connection lost. Retrying…';
        $('statusContent').dataset.stale = 'true';
        $('bridgeState').textContent = 'Connection lost';
        $('bridgeState').dataset.state = 'warning';
        $('bridgeDetail').textContent = 'The last status may be stale.';
    }

    function markStatusRestored() {
        if (!statusLost) return;
        statusLost = false;
        $('globalNotice').hidden = true;
        $('statusContent').dataset.stale = 'false';
    }

    async function fetchStatus() {
        const response = await fetch('/api/status', { cache: 'no-store' });
        if (!response.ok) {
            throw new Error('status unavailable');
        }
        const status = await response.json();
        markStatusRestored();
        renderStatus(status);
        return status;
    }

    async function refreshStatusNow() {
        try {
            await fetchStatus();
        } catch {
            if (statusSeen) markStatusLost();
        }
    }

    function stopStatusPolling() {
        if (statusTimer !== null) {
            window.clearTimeout(statusTimer);
            statusTimer = null;
        }
    }

    async function pollStatus() {
        stopStatusPolling();
        try {
            await fetchStatus();
        } catch {
            if (statusSeen) markStatusLost();
        } finally {
            statusTimer = window.setTimeout(pollStatus, statusLost ? 1500 : statusPollMs);
        }
    }

    function terminalWebSocketUrl() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        return `${protocol}//${window.location.host}/terminal`;
    }

    function setTerminalConnection(iconName, text, state, spinning = false) {
        setIcon($('terminalConnectionIcon'), iconName);
        $('terminalConnectionIcon').closest('svg').classList.toggle('spin', spinning);
        $('terminalConnectionText').textContent = text;
        $('terminalConnection').dataset.state = state;
    }

    function tcpOwnsSerial() {
        return lastStatus?.tcpState === 'connected';
    }

    function canSendTerminalBytes() {
        return terminalSocket?.readyState === WebSocket.OPEN && auth.authenticated && auth.terminalAvailable && !tcpOwnsSerial();
    }

    function updateTerminalWriteAccess() {
        const socketConnected = terminalSocket?.readyState === WebSocket.OPEN;
        const canTransmit = canSendTerminalBytes();
        const output = $('terminalOutput');
        const hint = $('terminalKeyboardHint');
        $('terminalSendInput').disabled = !canTransmit;
        $('terminalSend').disabled = !canTransmit;

        if (!socketConnected) {
            hint.textContent = 'Waiting for terminal connection.';
            output.setAttribute('aria-label', 'Serial terminal output.');
            return;
        }
        if (tcpOwnsSerial()) {
            setTerminalConnection('lock', 'Read-only · TCP connection is active', 'warning');
            hint.textContent = 'Read-only while the TCP connection is active.';
            output.setAttribute('aria-label', 'Read-only serial terminal output while TCP is connected.');
            return;
        }
        setTerminalConnection('circle-check', 'Connected', 'success');
        hint.textContent = 'Type directly; Tab moves focus. Pause affects display only.';
        output.setAttribute('aria-label', 'Serial terminal output. Focus here to type directly to the serial port.');
    }

    function updateTerminalCounters() {
        $('terminalCounters').textContent = `RX ${formatBytes(terminalRxBytes)} · TX ${formatBytes(terminalTxBytes)}`;
    }

    function rememberTerminalBytes(bytes) {
        for (const byte of bytes) terminalHistory.push(byte);
        const excess = terminalHistory.length - terminalHistoryLimit;
        if (excess > 0) terminalHistory.splice(0, excess);
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
                if (character === '\n') continue;
            }
            if (character === '\r') {
                carriageReturn = true;
                continue;
            }
            if (character === '\b') {
                if (rendered && !rendered.endsWith('\n')) rendered = rendered.slice(0, -1);
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
        if (carriageReturn) rendered += '\n';
        return rendered;
    }

    function terminalHex(bytes) {
        const lines = [];
        for (let offset = 0; offset < bytes.length; offset += 16) {
            const line = bytes.subarray(offset, offset + 16);
            const hex = Array.from(line, (byte) => byte.toString(16).padStart(2, '0').toUpperCase()).join(' ');
            const ascii = Array.from(line, (byte) => byte >= 0x20 && byte <= 0x7e ? String.fromCharCode(byte) : '.').join('');
            lines.push(`${hex.padEnd(47, ' ')}  ${ascii}`);
        }
        return lines.join('\n');
    }

    function renderTerminal() {
        if (terminalPaused) return;
        const output = $('terminalOutput');
        const follow = output.scrollHeight - output.scrollTop - output.clientHeight < 32;
        const bytes = terminalHistoryBytes();
        const hex = $('terminalMode').value === 'hex';
        output.classList.toggle('terminal-hex', hex);
        output.textContent = hex ? terminalHex(bytes) : terminalText(bytes);
        if (follow) output.scrollTop = output.scrollHeight;
    }

    function scheduleTerminalRender() {
        if (terminalPaused || terminalRenderPending) return;
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
            case 'cr': return new Uint8Array([0x0d]);
            case 'lf': return new Uint8Array([0x0a]);
            default: return new Uint8Array([0x0d, 0x0a]);
        }
    }

    function concatenateBytes(first, second) {
        const bytes = new Uint8Array(first.length + second.length);
        bytes.set(first, 0);
        bytes.set(second, first.length);
        return bytes;
    }

    function sendTerminalBytes(bytes, localEcho = true) {
        if (!canSendTerminalBytes() || bytes.length === 0) return false;
        if (bytes.length > terminalMaxFrameBytes) {
            announce(`Send is limited to ${terminalMaxFrameBytes.toLocaleString()} bytes per message.`);
            return false;
        }
        try {
            terminalSocket.send(bytes);
        } catch {
            return false;
        }
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
        if (sendTerminalBytes(bytes)) input.value = '';
    }

    function terminalControlByte(key) {
        if (key.length !== 1) return null;
        const code = key.toUpperCase().charCodeAt(0);
        return code >= 0x40 && code <= 0x5f ? code & 0x1f : null;
    }

    function sendTerminalKey(event) {
        if (!canSendTerminalBytes() || event.isComposing || event.metaKey || event.altKey) return;
        if (event.key === 'Tab') return;
        if (event.ctrlKey) {
            if (event.key.toLowerCase() === 'v') return;
            if (event.key.toLowerCase() === 'c' && window.getSelection()?.toString()) return;
            const byte = terminalControlByte(event.key);
            if (byte !== null) {
                event.preventDefault();
                sendTerminalBytes(new Uint8Array([byte]), false);
            }
            return;
        }

        const special = {
            Enter: { bytes: terminalLineEnding(), echo: true },
            Backspace: { bytes: new Uint8Array([0x08]), echo: true },
            Delete: { bytes: new Uint8Array([0x7f]), echo: false },
            Escape: { bytes: new Uint8Array([0x1b]), echo: false },
            ArrowUp: { bytes: new Uint8Array([0x1b, 0x5b, 0x41]), echo: false },
            ArrowDown: { bytes: new Uint8Array([0x1b, 0x5b, 0x42]), echo: false },
            ArrowRight: { bytes: new Uint8Array([0x1b, 0x5b, 0x43]), echo: false },
            ArrowLeft: { bytes: new Uint8Array([0x1b, 0x5b, 0x44]), echo: false }
        };
        if (special[event.key]) {
            event.preventDefault();
            sendTerminalBytes(special[event.key].bytes, special[event.key].echo);
            return;
        }
        if (event.key.length === 1) {
            event.preventDefault();
            sendTerminalBytes(terminalEncoder.encode(event.key));
        }
    }

    function sendTerminalPaste(event) {
        if (!canSendTerminalBytes()) return;
        const text = event.clipboardData?.getData('text');
        if (!text) return;
        event.preventDefault();
        sendTerminalBytes(terminalEncoder.encode(text));
    }

    function toggleTerminalPause() {
        terminalPaused = !terminalPaused;
        $('browserTerminal').dataset.paused = String(terminalPaused);
        $('terminalPauseText').textContent = terminalPaused ? 'Resume' : 'Pause';
        setIcon($('terminalPauseIcon'), terminalPaused ? 'play' : 'pause');
        if (!terminalPaused) renderTerminal();
    }

    function clearTerminal() {
        terminalHistory.length = 0;
        terminalRxBytes = 0;
        terminalTxBytes = 0;
        updateTerminalCounters();
        renderTerminal();
    }

    function scheduleTerminalReconnect() {
        if (!terminalWanted || terminalClosing || terminalReconnectTimer !== null) return;
        terminalReconnectTimer = window.setTimeout(() => {
            terminalReconnectTimer = null;
            connectTerminal();
        }, terminalReconnectDelayMs);
        terminalReconnectDelayMs = Math.min(terminalReconnectDelayMs * 2, 8000);
    }

    function connectTerminal() {
        if (
            !terminalWanted ||
            !auth.authenticated ||
            !auth.terminalAvailable ||
            terminalSocket?.readyState === WebSocket.OPEN ||
            terminalSocket?.readyState === WebSocket.CONNECTING
        ) {
            return;
        }

        terminalClosing = false;
        setTerminalConnection('loader-circle', 'Connecting…', 'warning', true);
        const socket = new WebSocket(terminalWebSocketUrl());
        terminalSocket = socket;
        socket.binaryType = 'arraybuffer';

        socket.addEventListener('open', () => {
            if (terminalSocket !== socket) return;
            terminalReconnectDelayMs = 1000;
            updateTerminalWriteAccess();
        });
        socket.addEventListener('message', async (event) => {
            if (terminalSocket !== socket) return;
            if (event.data instanceof ArrayBuffer) {
                receiveTerminalBytes(new Uint8Array(event.data));
            } else if (event.data instanceof Blob) {
                receiveTerminalBytes(new Uint8Array(await event.data.arrayBuffer()));
            }
        });
        socket.addEventListener('close', () => {
            if (terminalSocket === socket) terminalSocket = null;
            updateTerminalWriteAccess();
            if (!terminalClosing && terminalWanted) {
                setTerminalConnection('loader-circle', 'Reconnecting…', 'warning', true);
                scheduleTerminalReconnect();
            }
        });
        socket.addEventListener('error', () => {
            // close/reconnect owns visible connection state
        });
    }

    function stopTerminal() {
        terminalWanted = false;
        terminalClosing = true;
        if (terminalReconnectTimer !== null) {
            window.clearTimeout(terminalReconnectTimer);
            terminalReconnectTimer = null;
        }
        if (terminalSocket) {
            const socket = terminalSocket;
            terminalSocket = null;
            try { socket.close(); } catch { /* no-op */ }
        }
        setTerminalConnection('loader-circle', 'Disconnected', 'warning', false);
        updateTerminalWriteAccess();
        terminalClosing = false;
    }

    function initializeConfiguration() {
        $('configuration').addEventListener('submit', saveConfiguration);
        $('changeNetwork').addEventListener('click', openNetworkChooser);
        $('chooseNetwork').addEventListener('click', openNetworkChooser);
        $('enterNetwork').addEventListener('click', () => showManualNetwork(false));
        $('otherNetwork').addEventListener('click', () => showManualNetwork(false));
        $('cancelNetworkChange').addEventListener('click', cancelNetworkEdit);
        $('cancelManualNetwork').addEventListener('click', cancelNetworkEdit);
        $('scanAgain').addEventListener('click', scanNetworks);
        $('changePassword').addEventListener('click', changePassword);
        $('cancelPasswordChange').addEventListener('click', cancelPasswordChange);
        $('showPassword').addEventListener('click', () => setPasswordShown($('wifiPassword').type === 'password'));

        ['ssid', 'wifiSecurity', 'wifiPassword'].forEach((id) => {
            $(id).addEventListener('input', () => {
                if (wifiMode === 'manual') manualNetworkEdited = true;
                if (id === 'ssid' || id === 'wifiSecurity') {
                    if ($('wifiSecurity').value === 'open') {
                        $('wifiPassword').value = '';
                        passwordEditing = false;
                    } else {
                        passwordEditing =
                            Boolean($('wifiPassword').value) ||
                            networkIdentityChanged() ||
                            !wifiPasswordSaved;
                    }
                    renderPasswordEditor();
                }
                setFieldError(id);
                clearSaveFeedback();
                renderWifiSummary();
                refreshSaveState();
            });
        });

        document.querySelectorAll('input[name="tcpMode"]').forEach((input) => {
            input.addEventListener('change', () => {
                setFieldError('tcpMode');
                clearSaveFeedback();
                renderTcpFields();
                refreshSaveState();
            });
        });

        [
            'tcpListenPort',
            'tcpRemoteHost',
            'tcpRemotePort',
            'baud',
            'framing',
            'longPressMs',
            'longPressRepeatMs',
            'screenSaverSeconds'
        ].forEach((id) => {
            $(id).addEventListener('input', () => {
                setFieldError(id);
                clearSaveFeedback();
                if (id === 'screenSaverSeconds') updateScreenSaverPresentation();
                refreshSaveState();
            });
            $(id).addEventListener('change', () => {
                setFieldError(id);
                clearSaveFeedback();
                if (id === 'screenSaverSeconds' && fieldNumber(id) === 0) {
                    $(id).value = '';
                    updateScreenSaverPresentation();
                }
                refreshSaveState();
            });
        });
    }

    function initializeSecurity() {
        $('loginButton').addEventListener('click', login);
        $('loginPassword').addEventListener('keydown', (event) => {
            if (event.key === 'Enter') login();
        });
        $('bootstrapButton').addEventListener('click', bootstrapPassword);
        $('bootstrapPasswordConfirm').addEventListener('keydown', (event) => {
            if (event.key === 'Enter') bootstrapPassword();
        });

        document.querySelectorAll('[data-password-toggle]').forEach((button) => {
            button.addEventListener('click', () => {
                const input = $(button.dataset.passwordToggle);
                setPasswordToggle(button, input?.type === 'password');
            });
        });

        $('openPasswordChange').addEventListener('click', () => {
            setPasswordChangeError();
            setPasswordChangeFeedback();
            $('passwordDialog').showModal();
            $('currentAdminPassword').focus();
        });
        $('cancelPasswordUpdate').addEventListener('click', closePasswordDialog);
        $('passwordDialog').addEventListener('cancel', (event) => {
            event.preventDefault();
            closePasswordDialog();
        });
        $('updatePassword').addEventListener('click', updateAdminPassword);
        $('logoutButton').addEventListener('click', logout);
    }

    function initializeTerminal() {
        $('terminalSendForm').addEventListener('submit', sendTerminalLine);
        $('terminalOutput').addEventListener('keydown', sendTerminalKey);
        $('terminalOutput').addEventListener('paste', sendTerminalPaste);
        $('terminalPause').addEventListener('click', toggleTerminalPause);
        $('terminalClear').addEventListener('click', clearTerminal);
        $('terminalMode').addEventListener('change', renderTerminal);
        updateTerminalCounters();
        updateTerminalWriteAccess();
    }

    function scheduleAuthRetry() {
        if (authRetryTimer !== null || authAvailable) return;
        authRetryTimer = window.setTimeout(retryAuth, 1500);
    }

    async function retryAuth() {
        authRetryTimer = null;
        try {
            await fetchAuth();
            applyAccessModel(auth.authenticated);
        } catch {
            authAvailable = false;
            auth.authenticated = false;
            applyAccessModel(false);
            scheduleAuthRetry();
        }
    }

    async function initialize() {
        initializeTabs();
        initializeConfiguration();
        initializeSecurity();
        initializeTerminal();

        try {
            await fetchStatus();
        } catch {
            $('bootView').innerHTML = '';
            $('bootView').append(createIcon('triangle-alert'), document.createTextNode('Could not reach the device. Retrying…'));
            window.setTimeout(initializeConnection, 1500);
            return;
        }

        try {
            await fetchAuth();
        } catch {
            authAvailable = false;
            auth.authenticated = false;
            scheduleAuthRetry();
        }
        applyAccessModel(auth.authenticated);
        pollStatus();
    }

    async function initializeConnection() {
        try {
            await fetchStatus();
        } catch {
            window.setTimeout(initializeConnection, 1500);
            return;
        }

        try {
            await fetchAuth();
        } catch {
            authAvailable = false;
            auth.authenticated = false;
            scheduleAuthRetry();
        }
        applyAccessModel(auth.authenticated);
        pollStatus();
    }

    initialize();
})();
