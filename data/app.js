(() => {
    const $ = (id) => document.getElementById(id);

    // en.json is the one maintained source for user-facing text. strings.js is
    // generated from it and loaded before this file, so the catalog costs no
    // request of its own.
    const catalog = window.strings || {};

    // A key the catalog cannot answer is a build fault, and tools/build_strings.py
    // fails the build on one. This only has to keep such a page reading as
    // English instead of as a key path.
    function fallbackText(key) {
        const words = key.split('.').pop().replace(/([a-z0-9])([A-Z])/g, '$1 $2').toLowerCase();
        return words.charAt(0).toUpperCase() + words.slice(1);
    }

    // Named placeholders, as in {network}. A value the caller did not supply is
    // dropped rather than printed, because a placeholder name is not English.
    function t(key, values = {}) {
        const message = catalog[key];
        if (message === undefined) return fallbackText(key);
        return message.replace(/\{(\w+)\}/g,
            (placeholder, name) => (name in values ? String(values[name]) : ''));
    }

    // Every authored string in index.html is a key. This is where the catalog
    // answers them, once, before anything is shown.
    function applyStrings() {
        document.querySelectorAll('[data-i18n]').forEach((element) => {
            element.textContent = t(element.dataset.i18n);
        });
        document.querySelectorAll('[data-i18n-label]').forEach((element) => {
            element.setAttribute('aria-label', t(element.dataset.i18nLabel));
        });
        document.querySelectorAll('[data-i18n-placeholder]').forEach((element) => {
            element.setAttribute('placeholder', t(element.dataset.i18nPlaceholder));
        });
    }

    const statusPollMs = 1000;
    const trialPollMs = 1000;
    const terminalHistoryLimit = 32 * 1024;
    const terminalMaxFrameBytes = 1024;
    const terminalEncoder = new TextEncoder();
    const terminalDecoder = new TextDecoder();
    // Mirrors browser_terminal::Direction; the device prefixes every frame
    // with it. Entries are {value, direction} so a direction can be filtered
    // out of the view without being lost from history.
    const fromSerial = 0;
    const toSerial = 1;
    const terminalHistory = [];
    // The same tags the OLED prints, so both surfaces read alike.
    const terminalTag = (direction) => direction === toSerial ? '<S' : 'S>';

    let auth = {
        passwordSet: false,
        authenticated: false,
        terminalAvailable: false
    };
    let authAvailable = false;
    let authRetryTimer = null;
    let authCsrfToken = '';
    let authDialogNotice = null;
    let authRequestPending = false;
    let interruptedView = null;
    let configCsrfToken = '';
    let configurationBaseline = null;
    // The stored network, held apart from the form. Credentials are only ever
    // changed by a trial that proves them first, so an ordinary settings save
    // posts this network unchanged; the device rejects anything else.
    let savedNetwork = { ssid: '', wifiSecurity: 'unset' };
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
    // Back appears only when the network list is where this edit started.
    // Changing the password of the saved network does not pass through it, and
    // a LAN session has no list at all. Derived on entry so no path can leave
    // it stale.
    let networkEditFromChooser = false;
    let scanTimer = null;
    // True while a scan has been asked for and no result has arrived. The
    // footer offers Scan for a finished list, not during the wait.
    let scanPending = false;
    // The last /api/wifi/trial payload this page has seen, and when the attempt
    // now on screen was first observed. Both are cleared when the panel closes.
    let lastTrial = null;
    let trialStartedAt = 0;
    let trialTimer = null;

    let terminalWanted = false;
    let terminalSocket = null;
    let terminalReconnectTimer = null;
    let terminalReconnectDelayMs = 1000;
    let terminalRetryAt = 0;
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

    function storedTheme() {
        try {
            return localStorage.getItem('theme');
        } catch (error) {
            return null;
        }
    }

    function applyTheme(theme) {
        document.documentElement.dataset.theme = theme;
        const dark = theme === 'dark';
        // The button offers the other theme, so it carries that theme's icon.
        $('themeIcon').setAttribute('href', dark ? '#icon-sun' : '#icon-moon');
        $('themeToggle').setAttribute(
            'aria-label', t(dark ? 'theme.switchToLight' : 'theme.switchToDark'));
    }

    function resolveTheme() {
        // Mirrors the inline script in the document head, which has to run
        // before the first paint and so cannot call in here. This copy also
        // repairs the theme if that script was blocked: without it a missing
        // attribute would pin a dark-system reader to the light palette.
        const stored = storedTheme();
        if (stored === 'dark' || stored === 'light') return stored;
        return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
    }

    function initializeTheme() {
        const current = document.documentElement.dataset.theme;
        applyTheme(current === 'dark' || current === 'light' ? current : resolveTheme());
        $('themeToggle').addEventListener('click', () => {
            const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
            try {
                localStorage.setItem('theme', next);
            } catch (error) {
                // A browser that refuses storage still switches for this visit.
            }
            applyTheme(next);
        });
        // Until a choice is made the page follows the system, including a
        // change made while it is open. A terminal session can stay open for
        // hours, long enough to cross the system's own light/dark switch.
        const systemTheme = window.matchMedia('(prefers-color-scheme: dark)');
        // Older WebViews carry MediaQueryList without addEventListener. This
        // runs before every other initializer, so an exception here would take
        // the whole page down over a convenience.
        if (typeof systemTheme.addEventListener !== 'function') return;
        systemTheme.addEventListener('change', (event) => {
            if (storedTheme() === null) applyTheme(event.matches ? 'dark' : 'light');
        });
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

    // The settings form only. The Wi-Fi editor's fields belong to the next
    // attempt, not to this save, and clearWifiFieldErrors owns them.
    function clearFieldErrors() {
        [
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

    function clearWifiFieldErrors() {
        ['ssid', 'wifiSecurity', 'wifiPassword'].forEach((field) => setFieldError(field));
    }

    function setAuthError(message = '') {
        ['authError', 'authBootstrapError'].forEach((id) => {
            const error = $(id);
            error.textContent = message;
            error.hidden = !message;
        });
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
        setIcon(button.querySelector('use'), shown ? 'eye-off' : 'eye');
        // The authored aria-label names the field; only the pressed state changes.
        button.setAttribute('aria-pressed', String(shown));
    }

    function hidePassword(id) {
        const input = $(id);
        if (!input) return;
        input.type = 'password';
        document.querySelectorAll(`[data-password-toggle="${id}"]`).forEach((button) => {
            setPasswordToggle(button, false);
        });
    }

    function clearAdminPasswordFields() {
        ['currentAdminPassword', 'newAdminPassword', 'confirmAdminPassword'].forEach((id) => {
            $(id).value = '';
            hidePassword(id);
        });
    }

    function clearAuthDialogSecrets() {
        ['loginPassword', 'bootstrapPassword', 'bootstrapPasswordConfirm'].forEach((id) => {
            $(id).value = '';
            hidePassword(id);
        });
        setAuthError();
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
            throw new Error(t('errors.authStateUnavailable'));
        }
        const state = await response.json();
        auth = {
            passwordSet: Boolean(state.passwordSet),
            authenticated: Boolean(state.authenticated),
            terminalAvailable: Boolean(state.terminalAvailable)
        };
        authAvailable = true;
        authCsrfToken = state.csrfToken || '';
        authStateChanged();
        return auth;
    }

    function authDialogMode() {
        if (!authAvailable) return 'unavailable';
        if (!auth.passwordSet && auth.terminalAvailable) return 'bootstrap';
        if (!auth.passwordSet) return 'not-configured';
        return 'login';
    }

    function renderAuthDialog() {
        const mode = authDialogMode();
        const dialog = $('authDialog');
        // dataset.mode records what the dialog currently shows, so live auth
        // changes can tell a real mode transition from a routine re-render.
        const modeChanged = dialog.dataset.mode !== mode;
        dialog.dataset.mode = mode;
        $('authLoginFields').hidden = mode !== 'login';
        $('authBootstrapFields').hidden = mode !== 'bootstrap';
        $('closeAuthDialog').disabled = authRequestPending;

        let title = t('auth.signIn.title');
        let text = '';
        if (mode === 'unavailable') {
            title = t('auth.unavailable.title');
            text = t('auth.unavailable.text');
        } else if (mode === 'bootstrap') {
            title = t('auth.bootstrap.title');
        } else if (mode === 'not-configured') {
            title = t('auth.notConfigured.title');
            text = t('auth.notConfigured.text');
        } else {
            title = authDialogNotice?.title || title;
            text = authDialogNotice?.text || text;
        }

        $('authDialogTitle').textContent = title;
        // Several modes say everything in the title; the paragraph goes away
        // rather than reserving a blank line under it.
        $('authDialogText').textContent = text;
        $('authDialogText').hidden = !text;
        return modeChanged;
    }

    // Single owner of "auth state changed while the dialog is open": every path
    // that mutates auth (the /api/auth fetch and the /api/status poll) funnels
    // here, so the dialog can never show a form the backend would reject, and
    // focus follows only real mode transitions instead of routine re-renders.
    function authStateChanged() {
        if (!$('authDialog').open) return;
        if (renderAuthDialog()) focusAuthDialog();
    }

    function showPublicStatus() {
        selectedView = 'statusView';
        document.body.classList.remove('terminal-active');
        $('setupView').hidden = true;
        $('terminalView').hidden = true;
        $('statusView').hidden = false;
    }

    function setStatusPanelSemantics(authenticated) {
        if (authenticated) {
            $('statusView').setAttribute('role', 'tabpanel');
            $('statusView').setAttribute('aria-labelledby', 'statusTab');
        } else {
            $('statusView').removeAttribute('role');
            $('statusView').removeAttribute('aria-labelledby');
        }
    }

    function configurationIsDirty() {
        return configurationLoaded && !sameConfiguration(configurationValues(), configurationBaseline);
    }

    function rememberInterruptedWorkflow() {
        if (selectedView === 'setupView' || selectedView === 'terminalView') {
            interruptedView = selectedView;
        } else if (configurationIsDirty()) {
            interruptedView = 'setupView';
        }
    }

    function applyAccessModel(preferSetup = false) {
        const authenticated = authAvailable && auth.authenticated;

        $('bootView').hidden = true;
        $('navigation').hidden = !authenticated;
        $('setupTab').hidden = !authenticated;
        $('statusTab').hidden = !authenticated;
        // The Status tab titles this view once it exists. Signed out there are
        // no tabs, so the heading is the only title the page has.
        $('bridgeTitle').hidden = authenticated;
        $('terminalTab').hidden = !(authenticated && auth.terminalAvailable);
        $('securitySection').hidden = !authenticated;
        $('buildGroup').hidden = !authenticated;
        $('chooseNetwork').hidden = !auth.terminalAvailable;
        $('loginButton').hidden = authenticated;
        $('logoutButton').hidden = !authenticated;
        setStatusPanelSemantics(authenticated);

        if (!authenticated) {
            stopTerminal();
            document.body.classList.remove('terminal-active');
            if ($('passwordDialog').open) closePasswordDialog();
            showPublicStatus();
            return;
        }

        const resumeView = interruptedView;
        interruptedView = null;
        if (resumeView && visibleTabs().some((tab) => tab.dataset.view === resumeView)) {
            selectView(resumeView);
        } else if (preferSetup) {
            selectView('setupView');
        } else if (!selectedView || !visibleTabs().some((tab) => tab.dataset.view === selectedView)) {
            selectView('statusView');
        } else {
            selectView(selectedView);
        }
        refreshSaveState();
    }

    function focusAuthDialog() {
        if (!$('authDialog').open || authRequestPending) return;
        const mode = authDialogMode();
        const target = mode === 'login'
            ? $('loginPassword')
            : mode === 'bootstrap'
              ? $('bootstrapPassword')
              : $('closeAuthDialog');
        window.requestAnimationFrame(() => target.focus());
    }

    function openAuthDialog(notice = null) {
        authDialogNotice = notice;
        renderAuthDialog();
        if (!$('authDialog').open) $('authDialog').showModal();
        focusAuthDialog();
    }

    function closeAuthDialog() {
        if (authRequestPending) return;
        clearAuthDialogSecrets();
        authDialogNotice = null;
        if ($('authDialog').open) $('authDialog').close();
    }

    function setAuthRequestPending(pending) {
        authRequestPending = pending;
        $('authSubmit').disabled = pending;
        $('bootstrapButton').disabled = pending;
        $('closeAuthDialog').disabled = pending;
    }

    // Losing authentication must not destroy the configuration draft, so this
    // never calls clearProtectedSessionState(); only an intentional logout()
    // may discard the user's work.
    function handleAuthenticationLoss(notice, forcePrompt = false) {
        const protectedViewActive = selectedView === 'setupView' || selectedView === 'terminalView';
        rememberInterruptedWorkflow();
        stopScanPolling();
        // An interrupted scan must not resume presenting as still running when
        // the user returns to the chooser after signing back in.
        if (wifiMode === 'chooser') renderScanState('failed');
        applyAccessModel(false);
        if (forcePrompt || protectedViewActive) openAuthDialog(notice);
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

    // The firmware reports machine codes; people get sentences. The codes are
    // the protocol, so they stay here; the sentences live in the catalog.
    const authErrorMessages = {
        invalid_password: 'errors.invalidPassword',
        incorrect_password: 'errors.incorrectPassword',
        too_many_attempts: 'errors.tooManyAttempts',
        authentication_required: 'errors.authenticationRequired',
        password_required: 'errors.passwordRequired',
        password_already_set: 'errors.passwordAlreadySet',
        password_save_failed: 'errors.passwordSaveFailed',
        bootstrap_requires_setup_ap: 'auth.notConfigured.text',
        configuration_not_allowed: 'errors.notAllowedHere',
        csrf: 'errors.csrf'
    };

    function authErrorMessage(result, fallback) {
        const key = authErrorMessages[result.error];
        return key ? t(key) : fallback;
    }

    async function loginWithPassword(password) {
        const { response, result } = await postAuth('/api/auth/login', { password });
        if (!response.ok) {
            throw new Error(authErrorMessage(result, t('errors.invalidPassword')));
        }
        await fetchAuth();
        if (!auth.authenticated) {
            throw new Error(t('auth.signIn.incomplete'));
        }
    }

    async function login(event) {
        event?.preventDefault();
        if (authRequestPending) return;
        const password = $('loginPassword').value;
        if (!password) {
            setAuthError(t('auth.signIn.passwordRequired'));
            $('loginPassword').focus();
            return;
        }

        setAuthRequestPending(true);
        setAuthError();
        try {
            await loginWithPassword(password);
            $('loginPassword').value = '';
            setAuthRequestPending(false);
            closeAuthDialog();
            applyAccessModel(true);
            announce(t('auth.signIn.announced'));
        } catch (error) {
            // A failed request returns focus to the current mode's first field:
            // disabling the submit button dropped focus, and the mode may have
            // changed mid-request while the pending guard suppressed focus.
            setAuthRequestPending(false);
            setAuthError(error instanceof Error ? error.message : t('auth.signIn.failed'));
            focusAuthDialog();
        }
    }

    async function bootstrapPassword(event) {
        event?.preventDefault();
        if (authRequestPending) return;
        const password = $('bootstrapPassword').value;
        const confirm = $('bootstrapPasswordConfirm').value;

        if (!password) {
            setAuthError(t('auth.bootstrap.passwordRequired'));
            $('bootstrapPassword').focus();
            return;
        }
        if (password !== confirm) {
            setAuthError(t('auth.bootstrap.mismatch'));
            $('bootstrapPasswordConfirm').focus();
            return;
        }

        setAuthRequestPending(true);
        setAuthError();
        try {
            const { response, result } = await postAuth('/api/auth/password', {
                newPassword: password
            });
            if (!response.ok) {
                throw new Error(authErrorMessage(result, t('auth.bootstrap.failed')));
            }

            await fetchAuth();
            if (!auth.authenticated) {
                await loginWithPassword(password);
            }
            $('bootstrapPassword').value = '';
            $('bootstrapPasswordConfirm').value = '';
            setAuthRequestPending(false);
            closeAuthDialog();
            applyAccessModel(true);
            announce(t('auth.bootstrap.announced'));
        } catch (error) {
            // See login(): after a failed request, focus follows the rendered
            // mode — which here may have become "login" if the password was
            // created but the automatic sign-in failed.
            setAuthRequestPending(false);
            setAuthError(error instanceof Error ? error.message : t('auth.bootstrap.failed'));
            focusAuthDialog();
        }
    }

    // Discards the configuration draft, the terminal history and the baseline.
    // Only logout() may call this: an involuntary authentication loss must
    // leave the user's entered work intact.
    function clearProtectedSessionState() {
        configurationLoaded = false;
        configurationLoading = false;
        configurationBaseline = null;
        configCsrfToken = '';
        savedNetwork = { ssid: '', wifiSecurity: 'unset' };
        wifiPasswordSaved = false;
        stopTrialPolling();
        lastTrial = null;
        trialStartedAt = 0;
        clearFieldErrors();
        seedNetworkFields('', 'secured');
        setWifiMode('summary');
        $('configuration').hidden = true;
        $('saveRow').hidden = true;
        clearSaveFeedback();

        stopTerminal();
        terminalHistory.length = 0;
        terminalRxBytes = 0;
        terminalTxBytes = 0;
        terminalPaused = false;
        $('browserTerminal').dataset.paused = 'false';
        $('terminalSendInput').value = '';
        updateTerminalCounters();
        renderTerminal();
    }

    async function logout() {
        try {
            await postAuth('/api/auth/logout', {});
            await fetchAuth();
        } catch {
            announce(t('auth.signOut.failed'));
            return;
        }

        if (auth.authenticated) {
            announce(t('auth.signOut.failed'));
            return;
        }

        clearProtectedSessionState();
        applyAccessModel(false);
        announce(t('auth.signOut.announced'));
    }

    function requestLogout() {
        if (configurationIsDirty()) {
            $('signoutDialog').showModal();
            return;
        }
        logout();
    }

    async function updateAdminPassword(event) {
        event?.preventDefault();
        const currentPassword = $('currentAdminPassword').value;
        const newPassword = $('newAdminPassword').value;
        const confirmPassword = $('confirmAdminPassword').value;
        const button = $('updatePassword');

        setPasswordChangeError();
        setPasswordChangeFeedback();
        if (!currentPassword) {
            setPasswordChangeError(t('security.password.currentRequired'));
            $('currentAdminPassword').focus();
            return;
        }
        if (!newPassword) {
            setPasswordChangeError(t('security.password.newRequired'));
            $('newAdminPassword').focus();
            return;
        }
        if (newPassword !== confirmPassword) {
            setPasswordChangeError(t('security.password.mismatch'));
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
                throw new Error(authErrorMessage(result, t('security.password.failed')));
            }

            closePasswordDialog();

            try {
                await fetchAuth();
            } catch {
                authAvailable = false;
                scheduleAuthRetry();
            }

            auth.authenticated = false;
            handleAuthenticationLoss(
                { title: t('security.password.changedTitle') },
                true
            );
            announce(t('security.password.changedAnnounced'));
        } catch (error) {
            setPasswordChangeError(
                error instanceof Error ? error.message : t('security.password.failed'));
        } finally {
            button.disabled = false;
        }
    }

    // The network is deliberately absent: the Wi-Fi editor no longer stages a
    // change into this form, so nothing here can become a credential write.
    function configurationValues() {
        return {
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


    // The Wi-Fi section shows one of six things: the saved network, the scan
    // list, a password for a chosen network, a name typed by hand, an attempt
    // in flight, or the handoff after one succeeded. Nothing here stages a
    // change into the settings form; the network changes only when a trial
    // proves it, or when it is forgotten.
    function editingNetwork() {
        return wifiMode === 'credentials' || wifiMode === 'manual';
    }

    function trialFailure() {
        return lastTrial !== null && !lastTrial.running &&
            lastTrial.outcome !== 'none' && lastTrial.outcome !== 'connected';
    }

    function setWifiMode(mode) {
        if (mode === 'credentials' || mode === 'manual') {
            networkEditFromChooser = wifiMode === 'chooser';
        }
        wifiMode = mode;
        // Scan polling belongs to the chooser; it must not outlive it.
        if (mode !== 'chooser') stopScanPolling();
        $('wifiSummary').hidden = mode !== 'summary';
        $('wifiEditor').hidden = mode === 'summary';
        $('wifiChooser').hidden = mode !== 'chooser';
        $('wifiCredentials').hidden = mode !== 'credentials';
        $('manualNetworkFields').hidden = mode !== 'manual';
        $('wifiTrial').hidden = mode !== 'connecting' && mode !== 'connected';
        $('wifiCredentialsName').textContent = $('ssid').value || t('wifi.editor.networkHeading');
        // A message belongs to the step that produced it. Forget writes its
        // outcome after the step change below, so this cannot erase it.
        clearWifiFeedback();
        renderPasswordEditor();
        renderTrialMessage();
        renderLanTrialNotice();
        renderWifiActions();
    }

    // Back and the trial exit share one button, because only one of them is
    // ever the way out of the step on screen.
    function backFromEditor() {
        return editingNetwork() && networkEditFromChooser && !trialFailure();
    }

    // One footer serves every step. Back is the step behind this one and
    // Cancel is the way out of the flow, so a step reached from the list shows
    // both. A verdict replaces Cancel with the exit that also dismisses it,
    // because a verdict left behind is resurrected by the next page load.
    function renderWifiActions() {
        const failed = editingNetwork() && trialFailure();
        const connected = wifiMode === 'connected';
        const leaving = failed || connected || wifiMode === 'connecting';
        const back = backFromEditor();
        $('cancelNetworkChange').hidden =
            !(wifiMode === 'chooser' || (editingNetwork() && !failed));
        $('backToNetworks').hidden = !(leaving || back);
        // One button, three jobs: end the attempt in flight, step back to the
        // list, or close the flow. Label and icon are chosen together so that
        // they cannot drift apart.
        const exit = wifiMode === 'connecting' ? { text: t('common.cancel'), icon: 'x' }
            : back || (failed && auth.terminalAvailable) ? { text: t('common.back'), icon: 'arrow-left' }
            : { text: t('common.done'), icon: 'check' };
        $('backToNetworksText').textContent = exit.text;
        setIcon($('backToNetworksIcon'), exit.icon);
        $('scanAgain').hidden = wifiMode !== 'chooser' || scanPending;
        $('useNetwork').hidden = !editingNetwork();
        $('useNetworkText').textContent = t(failed ? 'common.retry' : 'common.connect');
        setIcon($('useNetworkIcon'), failed ? 'refresh-cw' : 'wifi');
    }

    function renderWifiSummary() {
        const configured = Boolean(savedNetwork.ssid);
        $('wifiConfigured').hidden = !configured;
        $('wifiEmpty').hidden = configured;
        $('wifiCurrentName').textContent = savedNetwork.ssid || '—';
        $('passwordSummary').hidden =
            !configured || savedNetwork.wifiSecurity !== 'secured';
        $('passwordState').textContent = t(wifiPasswordSaved ? 'wifi.password.saved' : 'wifi.password.notSet');
        // Named where the saved network is named, because a device that shows
        // what it remembers has to offer the way to make it forget.
        $('forgetNetwork').hidden = !configured;
        // Scanning is a setup-AP capability; from the LAN a network is typed.
        $('chooseNetwork').hidden = !auth.terminalAvailable;
        // From the LAN, Change opens the same typed editor Add would, so Add
        // appears only where it reaches something Change does not.
        $('enterNetwork').hidden = !auth.terminalAvailable && configured;
    }

    function renderPasswordEditor() {
        const visible = editingNetwork() && $('wifiSecurity').value === 'secured';
        $('passwordEditor').hidden = !visible;
        $('wifiPassword').disabled = !visible;
    }

    // Say it before starting, not after the page goes quiet: from the LAN the
    // attempt moves the device off the network this page is talking over.
    function renderLanTrialNotice() {
        const show = editingNetwork() && !auth.terminalAvailable;
        $('lanTrialNotice').hidden = !show;
        if (!show) return;
        const setup = lastStatus?.setupSsid;
        $('lanTrialNotice').textContent = setup
            ? t('wifi.editor.lanNoticeNamed', { setup })
            : t('wifi.editor.lanNoticeUnnamed');
    }

    // Every way into the editor starts from a known network, so a value left
    // behind by an abandoned edit can never be the one that gets attempted.
    function seedNetworkFields(ssid, security) {
        $('ssid').value = ssid;
        $('wifiSecurity').value = security === 'open' ? 'open' : 'secured';
        $('wifiPassword').value = '';
        hidePassword('wifiPassword');
        clearWifiFieldErrors();
    }

    function openNetworkChooser() {
        clearSaveFeedback();
        returnToNetworkChooser();
    }

    function returnToNetworkChooser() {
        if (!auth.terminalAvailable) {
            showManualNetwork(true);
            return;
        }
        setWifiMode('chooser');
        scanNetworks();
    }

    // Cancel leaves the Wi-Fi flow from any step of it. Nothing was staged, so
    // it changes nothing at all.
    function cancelNetworkEdit() {
        clearWifiFieldErrors();
        hidePassword('wifiPassword');
        setWifiMode('summary');
        renderWifiSummary();
    }

    // One step back, to the networks already found. A list left mid-scan is
    // the one thing there is nothing to come back to, so that case scans.
    function backToNetworkList() {
        clearWifiFieldErrors();
        hidePassword('wifiPassword');
        setWifiMode('chooser');
        if (scanPending) scanNetworks();
    }

    // Two ways in with two intents: correcting the network the device already
    // has, and naming one the scan cannot show.
    function showManualNetwork(seedSaved) {
        seedNetworkFields(
            seedSaved ? savedNetwork.ssid : '',
            seedSaved ? savedNetwork.wifiSecurity : 'secured');
        clearSaveFeedback();
        setWifiMode('manual');
        $('ssid').focus();
    }

    function selectNetwork(ssid, secured) {
        // Every network is confirmed before it is attempted, open or not: a
        // password that used to be reusable no longer is, because only a trial
        // may store one and a trial is given what the user typed.
        seedNetworkFields(ssid, secured ? 'secured' : 'open');
        clearSaveFeedback();
        setWifiMode('credentials');
        if (secured) $('wifiPassword').focus();
    }

    function changePassword() {
        seedNetworkFields(savedNetwork.ssid, savedNetwork.wifiSecurity);
        clearSaveFeedback();
        setWifiMode('credentials');
        $('wifiPassword').focus();
    }

    // 8 to 63 printable characters is what a WPA2 passphrase is; 64 is a raw
    // hex key, a different thing this product does not offer. Spaces are legal
    // anywhere in a passphrase, so nothing here trims what was typed, and a
    // line break is refused with a message rather than stripped in silence.
    function validateTrialFields() {
        clearWifiFieldErrors();
        const ssid = $('ssid').value;
        const password = $('wifiPassword').value;
        if (!ssid) {
            setFieldError('ssid', t('wifi.editor.ssidRequired'));
            $('ssid').focus();
            return null;
        }
        if ($('wifiSecurity').value !== 'secured') {
            return { ssid, wifiSecurity: 'open', wifiPassword: '' };
        }
        if (password.length < 8 || password.length > 63) {
            setFieldError('wifiPassword', t('wifi.password.length'));
            $('wifiPassword').focus();
            return null;
        }
        if (!/^[\x20-\x7e]+$/.test(password)) {
            setFieldError('wifiPassword', t('wifi.password.characters'));
            $('wifiPassword').focus();
            return null;
        }
        return { ssid, wifiSecurity: 'secured', wifiPassword: password };
    }

    function setTrialMessage(message) {
        $('trialMessage').hidden = !message;
        $('trialMessage').textContent = message || '';
    }

    function renderTrialMessage() {
        setTrialMessage(
            editingNetwork() && trialFailure() ? trialMessage(lastTrial) : '');
    }

    // One outcome, one message. None of these claims more than the device was
    // told: it never learns that a password was wrong, only which phase of the
    // connection did not complete.
    function trialMessage(trial) {
        const network = trial.ssid || t('wifi.network.fallback');
        switch (trial.outcome) {
            case 'auth_failed':
                return weakSignal(trial)
                    ? t('wifi.trial.authFailedWeakSignal', { network, rssi: Number(trial.rssi) })
                    : t('wifi.trial.authFailed', { network });
            case 'security_mismatch':
                return t('wifi.trial.securityMismatch', { network });
            case 'not_found':
                return t('wifi.trial.notFound', { network });
            case 'could_not_save':
                return t('wifi.trial.couldNotSave', { network });
            default:
                return t('wifi.trial.couldNotConnect', { network });
        }
    }

    // Whether the verdict may also name the signal: a true fact added to it,
    // never a competing explanation. A weak signal can lose a handshake, and so
    // can a wrong password. Zero means the failure carried no measurement.
    // -80 dBm, not the -67 or -70 quoted for good throughput. Those thresholds
    // describe a link that streams well; this sentence claims something much
    // narrower, that the four-way handshake can fail. A handshake is four small
    // unicast frames with a limited retry budget, and it still completes
    // reliably at -73. It starts genuinely timing out around -80, where the
    // link is at the edge of the radio's sensitivity. Saying "weak" any earlier
    // would hand the user a second explanation for a failure it did not cause.
    function weakSignal(trial) {
        const rssi = Number(trial.rssi);
        return Number.isFinite(rssi) && rssi !== 0 && rssi <= -80;
    }

    async function fetchTrial() {
        const response = await fetch('/api/wifi/trial', { cache: 'no-store' });
        if (!response.ok) throw new Error('trial unavailable');
        return response.json();
    }

    function stopTrialPolling() {
        if (trialTimer !== null) {
            window.clearTimeout(trialTimer);
            trialTimer = null;
        }
    }

    function startTrialPolling() {
        stopTrialPolling();
        trialTimer = window.setTimeout(pollTrial, trialPollMs);
    }

    // No timeout of its own: a verdict can take the device's full minute, and a
    // dropped request is expected here because the attempt may be taking down
    // the link this page is using.
    async function pollTrial() {
        trialTimer = null;
        try {
            await applyTrial(await fetchTrial());
        } catch {
            renderTrial();
            startTrialPolling();
        }
    }

    async function applyTrial(trial) {
        lastTrial = trial;
        if (trial.running) {
            if (wifiMode !== 'connecting') {
                trialStartedAt = Date.now();
                setWifiMode('connecting');
            }
            renderTrial();
            startTrialPolling();
            return;
        }
        stopTrialPolling();
        if (trial.outcome === 'connected') {
            await adoptConnectedTrial();
            return;
        }
        if (trial.outcome === 'none') {
            // Dismissed from somewhere else; there is nothing left to report.
            await leaveTrial();
            return;
        }
        showTrialVerdict();
    }

    // A page killed mid-attempt comes back to the verdict, not to a blank form.
    // Nothing to resume is the ordinary case and must stay silent.
    async function resumeTrial(trial) {
        if (!trial.running && trial.outcome === 'none') return;
        await applyTrial(trial);
    }

    async function startTrial() {
        const values = validateTrialFields();
        if (!values) return;

        setTrialMessage('');
        clearSaveFeedback();
        $('useNetwork').disabled = true;
        try {
            const response = await configFetch('/api/wifi/trial', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: new URLSearchParams(values).toString()
            });
            if (response.status === 401) {
                await handleSessionExpired();
                return;
            }
            const result = await safeJson(response);
            if (!response.ok) {
                if (result.field && $(`${result.field}Error`)) {
                    setFieldError(result.field, t('wifi.trial.rejectedValue'));
                    focusField(result.field);
                } else {
                    setTrialMessage(t('wifi.trial.startFailed'));
                }
                return;
            }
            // The device is already attempting; the panel takes over until it
            // reports how that ended.
            lastTrial = { outcome: 'none', running: true, ssid: values.ssid, rssi: 0 };
            trialStartedAt = Date.now();
            setWifiMode('connecting');
            renderTrial();
            startTrialPolling();
            announce(t('wifi.trial.announced', { network: values.ssid }));
        } catch {
            setTrialMessage(t('errors.unreachable'));
        } finally {
            $('useNetwork').disabled = false;
        }
    }

    // DELETE means one thing to the device: this trial no longer interests the
    // user. It cancels one that is running and dismisses the verdict of one
    // that is not, which is what stops a reload resurrecting an old failure.
    async function dismissTrial() {
        stopTrialPolling();
        lastTrial = null;
        trialStartedAt = 0;
        try {
            await configFetch('/api/wifi/trial', { method: 'DELETE' });
        } catch {
            // The panel closes either way, and the next trial replaces the
            // verdict on the device.
        }
    }

    async function leaveTrial() {
        const toNetworkList = auth.terminalAvailable && wifiMode !== 'connected';
        await dismissTrial();
        hidePassword('wifiPassword');
        clearWifiFieldErrors();
        if (toNetworkList) {
            returnToNetworkChooser();
            return;
        }
        setWifiMode('summary');
        renderWifiSummary();
    }

    // The verdict comes back to the editor with the network and the password
    // still in it. We never learned that either was wrong, so clearing either
    // for the user would be a claim we cannot support.
    function showTrialVerdict() {
        $('ssid').value = lastTrial.ssid;
        setWifiMode('manual');
        if (lastTrial.outcome === 'auth_failed') {
            // Revealed so they can check it themselves.
            document.querySelectorAll('[data-password-toggle="wifiPassword"]')
                .forEach((button) => setPasswordToggle(button, true));
        }
        renderTrial();
    }

    async function adoptConnectedTrial() {
        // The device has just committed a network this page has never seen, so
        // the baseline still describes the old one. Without this the summary
        // can name the previous network and the next ordinary save would post
        // a stale SSID.
        await refreshConfiguration();
        setWifiMode('connected');
        renderTrial();
        announce(t('wifi.trial.connectedAnnounced', { network: lastTrial.ssid }));
    }

    function renderTrial() {
        if (wifiMode === 'connecting') {
            const seconds = Math.max(0, Math.round((Date.now() - trialStartedAt) / 1000));
            $('trialTitle').textContent = t('wifi.trial.connecting',
                { network: lastTrial?.ssid || t('wifi.network.fallback') });
            $('trialDetail').textContent = t('wifi.trial.elapsed', { seconds });
            $('trialProgress').hidden = false;
            $('trialHandoff').hidden = true;
        } else if (wifiMode === 'connected') {
            renderHandoff();
        }
        renderTrialMessage();
        renderWifiActions();
    }

    function renderHandoff() {
        const network = lastTrial?.ssid || t('wifi.network.fallback');
        const address = lastTrial?.ip || '';
        $('trialProgress').hidden = true;
        $('trialTitle').textContent = t('wifi.trial.connected', { network });
        // Both gates, because either alone replays a stale card: an address
        // without the setup network is this page reached over the LAN hours
        // later, and the setup network without an address is a later dropout
        // answering a phone that joined it to find out what went wrong.
        const handoff = Boolean(lastTrial?.apActive) && Boolean(address);
        $('trialDetail').textContent = handoff ? '' : t('wifi.trial.saved');
        $('trialHandoff').hidden = !handoff;
        if (!handoff) return;
        // Where an address works is half the address: this one answers on the
        // network the device has just joined, and on no other.
        $('trialHandoffAddress').textContent = t('wifi.handoff.address', { network, address });
        // Only a reader on the setup network is about to lose it, so only that
        // reader is told to move. The deadline is stated as the rule the
        // firmware follows, not as time remaining, because a card reloaded
        // eight minutes in cannot know how much of the window is left.
        const setup = lastStatus?.setupSsid;
        $('trialHandoffNext').hidden = !auth.terminalAvailable;
        $('trialHandoffNext').textContent = auth.terminalAvailable
            ? t(setup ? 'wifi.handoff.nextNamed' : 'wifi.handoff.nextUnnamed',
                { setup, network, address })
            : '';
    }

    function signalIcon(rssi) {
        if (rssi >= -55) return 'wifi-high';
        if (rssi >= -72) return 'wifi-medium';
        return 'wifi-low';
    }

    function signalWord(rssi) {
        if (rssi >= -55) return t('wifi.signal.strong');
        if (rssi >= -72) return t('wifi.signal.good');
        if (rssi >= -82) return t('wifi.signal.fair');
        return t('wifi.signal.weak');
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
        scanPending = state === 'idle' || state === 'scanning';
        $('scanProgress').hidden = !scanPending;
        renderWifiActions();

        if (state === 'idle' || state === 'scanning') {
            $('scanState').textContent = t('wifi.scan.scanning');
            return;
        }
        if (state === 'failed') {
            $('scanState').textContent = t('wifi.scan.failed');
            return;
        }

        const visible = normalizeNetworks(networks);
        if (visible.length === 0) {
            $('scanState').textContent = t('wifi.scan.none');
            return;
        }
        $('scanState').textContent = visible.length === 1
            ? t('wifi.scan.foundOne')
            : t('wifi.scan.foundMany', { count: visible.length });

        visible.forEach((network) => {
            const row = document.createElement('button');
            row.type = 'button';
            row.className = 'network-row';
            const signal = signalWord(Number(network.rssi));
            row.setAttribute(
                'aria-label',
                t(network.secured ? 'wifi.chooser.rowSecured' : 'wifi.chooser.rowOpen',
                    { network: network.ssid, signal })
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
            meta.textContent = signal;
            const lock = createIcon(network.secured ? 'lock' : 'lock-open');
            // The two padlocks differ by a few pixels of shackle at this size,
            // so colour carries the difference too. The label still states it,
            // so nothing depends on colour alone.
            lock.classList.add(network.secured ? 'network-secured' : 'network-open');
            meta.appendChild(lock);

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
            if (response.status === 401) {
                scanTimer = null;
                await handleSessionExpired();
                return;
            }
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
            if (response.status === 401) {
                await handleSessionExpired();
                return;
            }
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

        savedNetwork = {
            ssid: config.ssid || '',
            wifiSecurity: config.wifiSecurity === 'open' ? 'open'
                : config.wifiSecurity === 'secured' ? 'secured' : 'unset'
        };
        // The editor's fields describe the next attempt, not the stored record.
        // They start from the stored network and are re-seeded on every way in.
        seedNetworkFields(savedNetwork.ssid, savedNetwork.wifiSecurity);
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
        setWifiMode('summary');
        renderWifiSummary();
        renderTcpFields();

        configurationBaseline = { ...values };
        refreshSaveState();
    }

    async function refreshConfiguration() {
        try {
            const response = await fetch('/api/config', { cache: 'no-store' });
            if (!response.ok) return;
            fillConfiguration(await response.json());
        } catch {
            // The summary keeps what it has; the next load corrects it.
        }
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

    // Forget answers beside the network it removes. The save row is three
    // sections further down the page, and an answer that far from the button
    // reads as no answer at all.
    function showWifiFeedback(message, state, icon) {
        $('wifiState').hidden = false;
        $('wifiState').dataset.state = state;
        $('wifiStateText').textContent = message;
        setIcon($('wifiStateIcon'), icon);
        // The spinner belongs to the message, not to the caller: the verdict
        // that replaces "Forgetting..." must stop it without being asked to.
        $('wifiStateIcon').closest('svg').classList.toggle('spin', icon === 'loader-circle');
    }

    function clearWifiFeedback() {
        $('wifiState').hidden = true;
        $('wifiStateText').textContent = '';
        $('wifiState').removeAttribute('data-state');
    }

    function validateNumberField(id, min, max) {
        const value = fieldNumber(id);
        if (!Number.isInteger(value) || value < min || value > max) {
            setFieldError(id, t('errors.range', { min, max }));
            return false;
        }
        return true;
    }

    function validateConfiguration(values) {
        clearFieldErrors();
        let valid = true;

        const listenPort = Number(values.tcpListenPort);
        const listenValid = Number.isInteger(listenPort) && listenPort >= 0 && listenPort <= 65535;
        if (!listenValid) {
            if (values.tcpMode === 'listen') {
                setFieldError('tcpListenPort', t('errors.portRange'));
            } else {
                setFieldError('tcpMode', t('errors.correctListenSettings'));
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
                    setFieldError('tcpRemoteHost', t('errors.serverRequired'));
                }
                if (hasRemoteHost && !hasRemotePort) {
                    setFieldError('tcpRemotePort', t('errors.remotePortRequired'));
                } else if (!remotePortInRange) {
                    setFieldError('tcpRemotePort', t('errors.portRange'));
                }
            } else {
                setFieldError('tcpMode', t('errors.correctConnectSettings'));
            }
            valid = false;
        }

        valid = validateNumberField('longPressMs', 100, 1000) && valid;
        valid = validateNumberField('longPressRepeatMs', 250, 1000) && valid;
        const screenSaver = fieldNumber('screenSaverSeconds');
        if (!Number.isInteger(screenSaver) || screenSaver < 0 || (screenSaver > 0 && screenSaver < 5)) {
            setFieldError('screenSaverSeconds', t('errors.screenSaverRange'));
            valid = false;
        }

        if (!valid) {
            const firstInvalid = document.querySelector('[aria-invalid="true"]');
            if (firstInvalid?.id) focusField(firstInvalid.id);
        }
        return valid;
    }

    function adoptPersistedConfiguration(values) {
        configurationBaseline = { ...values };
        refreshSaveState();
    }

    function backendErrorMessage(result, response) {
        if (result.error === 'credential_change_not_allowed') {
            // Only a page old enough to predate the trial can produce this.
            return t('errors.credentialChangeNotAllowed');
        }
        if (result.error === 'invalid_timeout') {
            return t('errors.invalidTimeout');
        }
        if (response.status === 400 && result.field) {
            return t('errors.invalidValue');
        }
        if (response.status === 400) {
            return t('errors.settingsInvalid');
        }
        if (response.status === 500) {
            return t('errors.settingsNotStored');
        }
        if (response.status === 403) {
            return t('errors.notAllowedHere');
        }
        return t('errors.saveFailed');
    }

    function showBackendFieldError(field, message) {
        setFieldError(field, message);
        const fieldMode = field === 'tcpListenPort'
            ? 'listen'
            : field === 'tcpRemoteHost' || field === 'tcpRemotePort'
              ? 'connect'
              : null;

        if (fieldMode && tcpModeValue() !== fieldMode) {
            setFieldError('tcpMode', t(fieldMode === 'listen'
                ? 'errors.correctListenSettings'
                : 'errors.correctConnectSettings'));
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
            scheduleAuthRetry();
        }
        auth.authenticated = false;
        handleAuthenticationLoss({
            title: t('auth.sessionEnded.title'),
            text: t('auth.sessionEnded.text')
        });
    }

    // The device accepts exactly two shapes here: the stored network unchanged,
    // and the forget shape. New credentials go to /api/wifi/trial instead, so
    // this never sends a password and never sends a different network.
    function postConfiguration(network, values) {
        return configFetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: new URLSearchParams({
                ssid: network.ssid,
                wifiSecurity: network.wifiSecurity,
                wifiPassword: '',
                tcpMode: values.tcpMode,
                tcpListenPort: values.tcpListenPort,
                tcpRemoteHost: values.tcpRemoteHost,
                tcpRemotePort: values.tcpRemotePort,
                baud: values.baud,
                framing: values.framing,
                longPressMs: values.longPressMs,
                longPressRepeatMs: values.longPressRepeatMs,
                screenSaverSeconds: values.screenSaverSeconds
            }).toString()
        });
    }

    // Forgetting touches the network and nothing else, so it posts the stored
    // settings rather than whatever the form currently holds.
    // Forgetting is not a credential write: it needs no trial and cannot fail
    // on the network, so it stays an ordinary settings save. It is confirmed
    // because of where it leaves the user, not because it is hard to undo: from
    // the LAN it drops the device off the network this page is reached over,
    // and the way back is the setup AP printed on the display.
    function askToForget() {
        $('forgetDialogDetail').textContent = t(
            auth.terminalAvailable ? 'wifi.forget.detailSetupAp' : 'wifi.forget.detailLan',
            { network: savedNetwork.ssid });
        $('forgetDialog').showModal();
    }

    async function forgetNetwork() {
        if (!configurationBaseline) return;
        const forgotten = { ssid: '', wifiSecurity: 'unset' };
        $('forgetNetwork').disabled = true;
        clearSaveFeedback();
        showWifiFeedback(t('wifi.forget.working'), 'warning', 'loader-circle');
        try {
            const response = await postConfiguration(forgotten, configurationBaseline);
            if (response.status === 401) {
                await handleSessionExpired();
                return;
            }
            if (!response.ok) {
                showWifiFeedback(t('wifi.forget.failed'), 'danger', 'circle-x');
                return;
            }
            savedNetwork = forgotten;
            wifiPasswordSaved = false;
            seedNetworkFields('', 'secured');
            setWifiMode('summary');
            renderWifiSummary();
            clearWifiFeedback();
            announce(t('wifi.forget.done'));
            window.setTimeout(refreshStatusNow, 250);
        } catch {
            showWifiFeedback(t('errors.unreachable'), 'danger', 'circle-x');
        } finally {
            $('forgetNetwork').disabled = false;
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
        $('saveText').textContent = t('common.saving');
        clearSaveFeedback();

        try {
            const response = await postConfiguration(savedNetwork, values);
            responseReceived = true;
            const result = await safeJson(response);

            if (response.status === 401) {
                await handleSessionExpired();
                return;
            }

            if (!response.ok) {
                if (result.error === 'serial_error' && result.persisted === true) {
                    adoptPersistedConfiguration(values);
                    showSaveFeedback(t('save.serialNotApplied'), 'warning', 'triangle-alert');
                    return;
                }

                if (result.field && $(`${result.field}Error`)) {
                    showBackendFieldError(result.field, backendErrorMessage(result, response));
                    showSaveFeedback(t('errors.saveFailed'), 'danger', 'circle-x');
                    return;
                }
                throw new Error(backendErrorMessage(result, response));
            }

            adoptPersistedConfiguration(values);
            showSaveFeedback(t('save.done'), 'success', 'circle-check');

            window.setTimeout(refreshStatusNow, 250);
        } catch (error) {
            if (!responseReceived) {
                showSaveFeedback(t('save.unconfirmed'), 'warning', 'triangle-alert');
            } else {
                showSaveFeedback(
                    error instanceof Error ? error.message : t('errors.saveFailed'),
                    'danger', 'circle-x');
            }
        } finally {
            savePending = false;
            $('saveText').textContent = t('common.save');
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
                throw new Error('settings unavailable');
            }
            const config = await response.json();
            // Read the trial before the Wi-Fi section is shown at all, so a
            // page killed mid-attempt never flashes a blank form on its way to
            // the verdict it came back for.
            const trial = await fetchTrial().catch(() => null);
            fillConfiguration(config);
            if (trial) await resumeTrial(trial);
            configurationLoaded = true;
            refreshSaveState();
            $('setupLoading').hidden = true;
            $('configuration').hidden = false;
            $('saveRow').hidden = false;
        } catch {
            $('setupLoading').hidden = true;
            $('setupUnavailable').hidden = false;
            $('saveRow').hidden = true;
        } finally {
            configurationLoading = false;
        }
    }

    function tcpModeText(mode) {
        return t(mode === 'connect' ? 'tcp.modeConnectLong' : 'tcp.modeListenLong');
    }

    function tcpStateText(state, mode) {
        switch (state) {
            case 'disabled': return t('tcp.state.disabled');
            case 'waiting_for_wifi': return t('tcp.state.waitingForWifi');
            case 'listening': return t('tcp.state.listening');
            case 'connecting': return t('tcp.state.connecting');
            case 'retrying': return t('tcp.state.retrying');
            case 'connected':
                return t(mode === 'listen' ? 'tcp.state.clientConnected' : 'tcp.state.connected');
            case 'failure': return t('tcp.state.failure');
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
                    ? { text: t('tcp.runtime.unavailable'), state: 'warning' }
                    : { text: t('tcp.runtime.notConfigured'), state: 'neutral' };
            case 'waiting_for_wifi':
                return { text: t('tcp.runtime.waitingForWifi'), state: 'warning' };
            case 'listening':
                return { text: t('tcp.runtime.listening', { port: listenPort }), state: 'success' };
            case 'connecting':
                return { text: t('tcp.runtime.connecting', { endpoint: remote }), state: 'warning' };
            case 'retrying':
                return { text: t('tcp.runtime.retrying', { endpoint: remote }), state: 'warning' };
            case 'connected':
                return mode === 'listen'
                    ? { text: t('tcp.runtime.clientConnected'), state: 'success' }
                    : { text: t('tcp.runtime.connected', { endpoint: remote }), state: 'success' };
            case 'failure':
                return { text: t('tcp.runtime.failure'), state: 'danger' };
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

    // How the device's last attempt on the saved network ended. It names the
    // phase that failed, never a cause the device was never told: the way out
    // is always the same, which is to connect again with a password it can
    // prove. The device keeps retrying on its own throughout.
    function wifiFailure(status) {
        switch (status.wifiOutcome) {
            case 'auth_failed':
                return {
                    title: t('status.wifi.authFailedTitle'),
                    state: 'danger',
                    detail: status.setupSsid
                        ? t('status.wifi.authFailedNamed', { setup: status.setupSsid })
                        : t('status.wifi.authFailedUnnamed')
                };
            case 'not_found':
                return {
                    title: t('status.wifi.notFoundTitle'),
                    state: 'danger',
                    detail: t('status.wifi.notFound')
                };
            case 'security_mismatch':
                return {
                    title: t('status.wifi.securityMismatchTitle'),
                    state: 'danger',
                    detail: t('status.wifi.securityMismatch')
                };
            case 'could_not_connect':
            case 'could_not_save':
                return {
                    title: t('status.wifi.notConnectedTitle'),
                    state: 'danger',
                    detail: t('status.wifi.notConnected')
                };
            default:
                // Only the danger outcomes reach renderBridgeState's detail; the
                // warning branch there states what TCP is waiting for instead.
                return { title: t('status.wifi.connectingTitle'), state: 'warning', detail: '' };
        }
    }

    function renderBridgeState(status) {
        const mode = status.tcpMode === 'connect' ? 'connect' : 'listen';
        const remote = endpoint(status.tcpRemoteHost, status.tcpRemotePort);
        const listenPort = Number(status.tcpListenPort) || 0;
        const tcpConfigured = mode === 'listen'
            ? listenPort > 0
            : Boolean(status.tcpRemoteHost) && Number(status.tcpRemotePort) > 0;
        let state = 'neutral';
        let title = t('common.notConfigured');
        let detail = '';

        if (!status.wifiConfigured || !tcpConfigured) {
            title = t('common.notConfigured');
            if (!status.wifiConfigured && !tcpConfigured) {
                detail = t('bridge.needsWifiAndTcp');
            } else if (!status.wifiConfigured) {
                detail = t('bridge.needsWifi');
            } else {
                detail = t(mode === 'listen' ? 'bridge.needsListenPort' : 'bridge.needsServer');
            }
        } else if (!status.wifiConnected) {
            const wifi = wifiFailure(status);
            title = wifi.title;
            state = wifi.state;
            // When there is something to correct, the instruction is the point;
            // the TCP note only competes with it.
            detail = wifi.state === 'danger'
                ? wifi.detail
                : t('bridge.wifiConnectingTcpWaiting');
        } else {
            switch (status.tcpState) {
                case 'disabled':
                    title = t('bridge.tcpUnavailableTitle');
                    break;
                case 'waiting_for_wifi':
                    title = t('bridge.waitingForWifiTitle');
                    state = 'warning';
                    break;
                case 'listening':
                    title = t('bridge.readyTitle');
                    state = 'success';
                    detail = t('bridge.ready', { port: listenPort.toLocaleString() });
                    break;
                case 'connecting':
                    title = t('bridge.connectingTitle');
                    state = 'warning';
                    detail = t('bridge.connecting', { endpoint: remote });
                    break;
                case 'retrying':
                    title = t('bridge.retryingTitle');
                    state = 'warning';
                    detail = t('bridge.retrying', { endpoint: remote });
                    break;
                case 'connected':
                    title = t('bridge.activeTitle');
                    state = 'success';
                    detail = mode === 'listen'
                        ? t('bridge.activeListen')
                        : t('bridge.activeConnect', { endpoint: remote });
                    break;
                case 'failure':
                    title = t('bridge.failureTitle');
                    state = 'danger';
                    detail = mode === 'listen'
                        ? t('bridge.failureListen')
                        : t('bridge.failureConnect', { endpoint: remote });
                    break;
                default:
                    title = t('bridge.unknownTitle');
                    state = 'warning';
            }
        }

        $('bridgeState').textContent = title;
        $('bridgeState').dataset.state = state;
        $('bridgeDetail').textContent = detail;
    }

    function renderStatus(status) {
        if (authAvailable) {
            const sessionEnded = auth.authenticated && !Boolean(status.authenticated);
            const deviceStateChanged =
                auth.passwordSet !== Boolean(status.passwordSet) ||
                auth.terminalAvailable !== Boolean(status.terminalAvailable);
            auth.passwordSet = Boolean(status.passwordSet);
            auth.terminalAvailable = Boolean(status.terminalAvailable);
            if (sessionEnded) {
                auth.authenticated = false;
                handleAuthenticationLoss({
                    title: t('auth.sessionEnded.title'),
                    text: t('auth.sessionEnded.text')
                });
            } else if (deviceStateChanged) {
                authStateChanged();
            }
        }
        lastStatus = status;
        statusSeen = true;
        $('firmwareBuild').textContent = status.firmwareBuild || '—';
        $('frontendBuild').textContent = status.frontendBuild || '—';
        if (status.setupSsid) {
            $('deviceName').textContent = status.setupSsid;
        }

        $('statusWifi').textContent = status.wifiConnected
            ? t('status.wifiConnected')
            : status.wifiConfigured
              ? wifiFailure(status).title
              : t('common.notConfigured');
        $('stationIp').textContent = status.stationIp && status.stationIp !== '0.0.0.0' ? status.stationIp : '—';
        $('configurationApState').textContent = t(status.wifiApActive ? 'status.setupApOn' : 'status.setupApOff');

        const mode = status.tcpMode === 'connect' ? 'connect' : 'listen';
        const listenPort = Number(status.tcpListenPort) || 0;
        const remotePort = Number(status.tcpRemotePort) || 0;
        const tcpConfigured = mode === 'listen'
            ? listenPort > 0
            : Boolean(status.tcpRemoteHost) && remotePort > 0;
        $('statusTcpMode').textContent = tcpModeText(mode);
        $('statusTcpState').textContent = status.tcpState === 'disabled' && !tcpConfigured
            ? t('common.notConfigured')
            : tcpStateText(status.tcpState, mode);
        if (mode === 'listen') {
            $('statusTcpEndpointLabel').textContent = t('status.tcpListenPortLabel');
            $('statusTcpEndpoint').textContent = listenPort ? listenPort.toLocaleString() : '—';
        } else {
            $('statusTcpEndpointLabel').textContent = t('status.tcpServerLabel');
            $('statusTcpEndpoint').textContent = endpoint(status.tcpRemoteHost, status.tcpRemotePort);
        }

        const baud = Number(status.baud) || 0;
        $('statusBaud').textContent = baud ? baud.toLocaleString() : '—';
        $('statusFraming').textContent = status.framing || '—';
        $('statusS2N').textContent = formatBytes(status.serialToNetworkReceived);
        $('statusN2S').textContent = formatBytes(status.networkToSerialReceived);
        $('statusDrops').textContent = t('status.dropped', {
            serialToTcp: (Number(status.serialToNetworkDropped) || 0).toLocaleString(),
            tcpToSerial: (Number(status.networkToSerialDropped) || 0).toLocaleString()
        });

        $('terminalSerialSettings').textContent = t('terminal.serialSettings', {
            baud: baud ? baud.toLocaleString() : '—',
            framing: status.framing || '—'
        });
        renderBridgeState(status);
        renderTcpRuntime(status);
        updateTerminalWriteAccess();
    }

    // The notice owns the lost connection; this line owns the bridge state,
    // which is now unknown, and the detail owns the age of what is on screen.
    function markStatusLost() {
        statusLost = true;
        $('globalNotice').hidden = false;
        $('statusContent').dataset.stale = 'true';
        $('bridgeState').textContent = t('bridge.unknownTitle');
        $('bridgeState').dataset.state = 'warning';
        $('bridgeDetail').textContent = t('bridge.stale');
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
        const hint = $('terminalKeyboardHint');
        $('terminalSendInput').disabled = !canTransmit;
        $('terminalSend').disabled = !canTransmit;

        // The chip owns the connection. The hint owns what the keyboard can do,
        // beside the controls it describes, so read-only is explained where the
        // disabled box is. With no connection there is nothing to describe.
        if (!socketConnected) {
            hint.textContent = '';
            return;
        }
        setTerminalConnection('circle-check', t('terminal.connected'), 'success');
        hint.textContent = t(tcpOwnsSerial() ? 'terminal.readOnlyHint' : 'terminal.typingHint');
    }

    function updateTerminalCounters() {
        $('terminalCounters').textContent = t('terminal.counters', {
            received: formatBytes(terminalRxBytes),
            sent: formatBytes(terminalTxBytes)
        });
    }

    function rememberTerminalBytes(bytes, direction) {
        for (const value of bytes) terminalHistory.push({ value, direction });
        const excess = terminalHistory.length - terminalHistoryLimit;
        if (excess > 0) terminalHistory.splice(0, excess);
    }

    function terminalEntries() {
        return $('terminalShowSent').checked ?
            terminalHistory : terminalHistory.filter((entry) => entry.direction === fromSerial);
    }

    // Bytes are shown in the order they happened, so a direction change starts
    // a new run. Runs, not frames, decide the marking: typing arrives one frame
    // per keystroke and must still read as one word.
    function terminalRuns(entries) {
        const runs = [];
        for (const entry of entries) {
            const last = runs[runs.length - 1];
            if (last && last.direction === entry.direction) last.bytes.push(entry.value);
            else runs.push({ direction: entry.direction, bytes: [entry.value] });
        }
        return runs;
    }

    // Backspace and CR act on whatever was printed last, whichever side sent
    // it, so the control-character rules run once over the whole stream and
    // each emitted character keeps the direction of the byte behind it.
    function terminalText(entries) {
        const pieces = [];
        let carriageReturn = false;
        let direction = fromSerial;
        for (const run of terminalRuns(entries)) {
            direction = run.direction;
            for (const character of terminalDecoder.decode(Uint8Array.from(run.bytes))) {
                if (carriageReturn) {
                    pieces.push({ text: '\n', direction });
                    carriageReturn = false;
                    if (character === '\n') continue;
                }
                if (character === '\r') {
                    carriageReturn = true;
                    continue;
                }
                if (character === '\b') {
                    const last = pieces[pieces.length - 1];
                    if (last && last.text !== '\n') pieces.pop();
                    continue;
                }
                if (character === '\n' || character === '\t') {
                    pieces.push({ text: character, direction });
                    continue;
                }
                if (character.codePointAt(0) >= 0x20 && character !== '\u007f') {
                    pieces.push({ text: character, direction });
                }
            }
        }
        if (carriageReturn) pieces.push({ text: '\n', direction });
        return pieces;
    }

    // Hex rows are already row-structured, so they carry the tag itself. A row
    // never mixes directions: the run ends the row even when it is short.
    function terminalHex(entries) {
        const rows = [];
        for (const run of terminalRuns(entries)) {
            const bytes = Uint8Array.from(run.bytes);
            for (let offset = 0; offset < bytes.length; offset += 16) {
                const line = bytes.subarray(offset, offset + 16);
                const hex = Array.from(line, (byte) => byte.toString(16).padStart(2, '0').toUpperCase()).join(' ');
                const ascii = Array.from(line, (byte) => byte >= 0x20 && byte <= 0x7e ? String.fromCharCode(byte) : '.').join('');
                const row = `${terminalTag(run.direction)} ${hex.padEnd(47, ' ')}  ${ascii}`;
                rows.push({
                    text: rows.length === 0 ? row : `\n${row}`,
                    direction: run.direction
                });
            }
        }
        return rows;
    }

    // Adjacent pieces of one direction become a single span, so the markup
    // holds one node per run rather than one per byte.
    function terminalSpans(pieces) {
        const spans = [];
        for (const piece of pieces) {
            const last = spans[spans.length - 1];
            if (last && last.dataset.direction === String(piece.direction)) {
                last.textContent += piece.text;
                continue;
            }
            const span = document.createElement('span');
            span.dataset.direction = String(piece.direction);
            if (piece.direction === toSerial) span.className = 'terminal-sent';
            span.textContent = piece.text;
            spans.push(span);
        }
        return spans;
    }

    function renderTerminal() {
        if (terminalPaused) return;
        const output = $('terminalOutput');
        const follow = output.scrollHeight - output.scrollTop - output.clientHeight < 32;
        const entries = terminalEntries();
        const hex = $('terminalMode').value === 'hex';
        output.classList.toggle('terminal-hex', hex);
        output.replaceChildren(
            ...terminalSpans(hex ? terminalHex(entries) : terminalText(entries)));
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

    function receiveTerminalFrame(frame) {
        // A frame is the direction byte plus at least one payload byte.
        if (frame.length < 2) return;
        const direction = frame[0] === toSerial ? toSerial : fromSerial;
        const bytes = frame.subarray(1);
        if (direction === toSerial) terminalTxBytes += bytes.length;
        else terminalRxBytes += bytes.length;
        rememberTerminalBytes(bytes, direction);
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

    function sendTerminalBytes(bytes) {
        if (!canSendTerminalBytes() || bytes.length === 0) return false;
        if (bytes.length > terminalMaxFrameBytes) {
            announce(t('terminal.sendLimit', { bytes: terminalMaxFrameBytes.toLocaleString() }));
            return false;
        }
        try {
            terminalSocket.send(bytes);
        } catch {
            return false;
        }
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
                sendTerminalBytes(new Uint8Array([byte]));
            }
            return;
        }

        const special = {
            Enter: terminalLineEnding(),
            Backspace: new Uint8Array([0x08]),
            Delete: new Uint8Array([0x7f]),
            Escape: new Uint8Array([0x1b]),
            ArrowUp: new Uint8Array([0x1b, 0x5b, 0x41]),
            ArrowDown: new Uint8Array([0x1b, 0x5b, 0x42]),
            ArrowRight: new Uint8Array([0x1b, 0x5b, 0x43]),
            ArrowLeft: new Uint8Array([0x1b, 0x5b, 0x44])
        };
        if (special[event.key]) {
            event.preventDefault();
            sendTerminalBytes(special[event.key]);
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
        $('terminalPauseText').textContent = t(terminalPaused ? 'common.resume' : 'common.pause');
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

    // The countdown owns no timer handle and is never cleared. Each tick is a
    // view of the pending attempt, so it stops itself as soon as that attempt
    // is gone or has been superseded: no exit path has to remember it.
    function tickRetryCountdown(retryAt) {
        if (terminalReconnectTimer === null || terminalRetryAt !== retryAt) return;
        renderTerminalRetryCountdown();
        window.setTimeout(() => tickRetryCountdown(retryAt), 500);
    }

    function renderTerminalRetryCountdown() {
        // The alert icon, the warning colour and the footer hint already say
        // the terminal is down, so this states only what happens next. It also
        // keeps the line short enough not to wrap at 375px.
        const seconds = Math.max(0, Math.ceil((terminalRetryAt - Date.now()) / 1000));
        setTerminalConnection('triangle-alert',
            seconds > 0 ? t('terminal.retryingIn', { seconds }) : t('terminal.retrying'), 'warning');
    }

    function scheduleTerminalReconnect() {
        if (!terminalWanted || terminalClosing || terminalReconnectTimer !== null) return;
        const delayMs = terminalReconnectDelayMs;
        terminalRetryAt = Date.now() + delayMs;
        terminalReconnectTimer = window.setTimeout(() => {
            terminalReconnectTimer = null;
            connectTerminal();
        }, delayMs);
        // Ticks twice a second, so the number shown is never more than half a
        // second behind the timer it describes.
        tickRetryCountdown(terminalRetryAt);
        terminalReconnectDelayMs = Math.min(delayMs * 2, 8000);
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
        setTerminalConnection('loader-circle', t('terminal.connecting'), 'warning', true);
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
                receiveTerminalFrame(new Uint8Array(event.data));
            } else if (event.data instanceof Blob) {
                receiveTerminalFrame(new Uint8Array(await event.data.arrayBuffer()));
            }
        });
        socket.addEventListener('close', () => {
            if (terminalSocket === socket) terminalSocket = null;
            updateTerminalWriteAccess();
            if (!terminalClosing && terminalWanted) {
                // The spinner belongs to an attempt in flight. Between attempts
                // the page is only waiting on a timer, so it counts that timer
                // down instead of animating progress that is not happening.
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
        setTerminalConnection('loader-circle', t('terminal.disconnected'), 'warning', false);
        updateTerminalWriteAccess();
        terminalClosing = false;
    }

    function initializeConfiguration() {
        $('configuration').addEventListener('submit', saveConfiguration);
        $('changeNetwork').addEventListener('click', openNetworkChooser);
        $('chooseNetwork').addEventListener('click', openNetworkChooser);
        // From the summary, manual entry corrects the network the device has;
        // from the chooser it names one the list cannot show.
        $('enterNetwork').addEventListener('click', () => showManualNetwork(true));
        $('otherNetwork').addEventListener('click', () => showManualNetwork(false));
        $('cancelNetworkChange').addEventListener('click', cancelNetworkEdit);
        $('backToNetworks').addEventListener('click', () => {
            if (backFromEditor()) backToNetworkList();
            else leaveTrial();
        });
        $('useNetwork').addEventListener('click', startTrial);
        $('forgetNetwork').addEventListener('click', askToForget);
        $('keepNetwork').addEventListener('click', () => $('forgetDialog').close());
        $('confirmForget').addEventListener('click', () => {
            $('forgetDialog').close();
            forgetNetwork();
        });
        $('scanAgain').addEventListener('click', scanNetworks);
        $('changePassword').addEventListener('click', changePassword);
        $('retrySetup').addEventListener('click', loadConfiguration);

        ['ssid', 'wifiSecurity', 'wifiPassword'].forEach((id) => {
            $(id).addEventListener('input', () => {
                setFieldError(id);
                if (id !== 'wifiPassword') {
                    $('wifiCredentialsName').textContent = $('ssid').value || t('wifi.editor.networkHeading');
                    renderPasswordEditor();
                    renderLanTrialNotice();
                }
                renderWifiActions();
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
        $('loginButton').addEventListener('click', () => openAuthDialog());
        $('authLoginFields').addEventListener('submit', login);
        $('authBootstrapFields').addEventListener('submit', bootstrapPassword);

        document.querySelectorAll('[data-password-toggle]').forEach((button) => {
            button.addEventListener('click', () => {
                const input = $(button.dataset.passwordToggle);
                setPasswordToggle(button, input?.type === 'password');
            });
        });

        $('closeAuthDialog').addEventListener('click', closeAuthDialog);
        $('authDialog').addEventListener('cancel', (event) => {
            event.preventDefault();
            closeAuthDialog();
        });
        $('authDialog').addEventListener('click', (event) => {
            if (event.target === $('authDialog')) closeAuthDialog();
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
        $('passwordChangeEditor').addEventListener('submit', updateAdminPassword);
        $('logoutButton').addEventListener('click', requestLogout);
        $('keepSession').addEventListener('click', () => $('signoutDialog').close());
        $('confirmSignout').addEventListener('click', () => {
            $('signoutDialog').close();
            logout();
        });
        $('signoutDialog').addEventListener('cancel', (event) => event.preventDefault());
    }

    function initializeTerminal() {
        $('terminalSendForm').addEventListener('submit', sendTerminalLine);
        $('terminalOutput').addEventListener('keydown', sendTerminalKey);
        $('terminalOutput').addEventListener('paste', sendTerminalPaste);
        $('terminalPause').addEventListener('click', toggleTerminalPause);
        $('terminalClear').addEventListener('click', clearTerminal);
        $('terminalMode').addEventListener('change', renderTerminal);
        $('terminalShowSent').addEventListener('change', renderTerminal);
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
            applyAccessModel(false);
        } catch {
            authAvailable = false;
            auth.authenticated = false;
            applyAccessModel(false);
            scheduleAuthRetry();
        }
    }

    async function initialize() {
        applyStrings();
        initializeTheme();
        initializeTabs();
        initializeConfiguration();
        initializeSecurity();
        initializeTerminal();

        try {
            await fetchStatus();
        } catch {
            $('bootView').innerHTML = '';
            $('bootView').append(
                createIcon('triangle-alert'), document.createTextNode(t('boot.unreachable')));
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
        applyAccessModel(false);
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
        applyAccessModel(false);
        pollStatus();
    }

    initialize();
})();
