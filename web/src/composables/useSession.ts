import { inject, provide, reactive, readonly, type InjectionKey } from 'vue';
import { CLIENT_TIMEOUTS, createApiClient, type ApiClient } from '../api/client';
import type {
  CapabilitiesData,
  Scope,
  ServerPhase,
  SessionData,
  SystemData,
} from '../api/generated';
import type { AppLifecycleState, SessionIdentity } from '../api/types';
import { ApiFailure } from '../api/errors';
import { durationToMsClamped } from '../format/time';

export interface NotificationItem {
  id: number;
  tone: 'info' | 'warning' | 'danger' | 'success';
  message: string;
}

export interface SessionStore {
  readonly lifecycle: AppLifecycleState;
  readonly identity: SessionIdentity | null;
  readonly serverPhase: ServerPhase | null;
  readonly systemSnapshot: SystemData | null;
  readonly capabilities: CapabilitiesData | null;
  readonly lastError: ApiFailure | null;
  readonly incompatibleReason: string | null;
  readonly intendedRoute: string | null;
  readonly notifications: readonly NotificationItem[];
  readonly api: ApiClient;
  readonly pollingFloorMs: number;
  readonly idempotencyRetentionMs: number;
  hasScope(scope: Scope): boolean;
  setIntendedRoute(path: string | null): void;
  pushNotification(tone: NotificationItem['tone'], message: string): void;
  dismissNotification(id: number): void;
  bootstrap(): Promise<void>;
  login(bearerToken: string): Promise<void>;
  logout(): Promise<void>;
  refreshCapabilities(): Promise<void>;
  updateServerPhaseFromSystem(sys: SystemData): void;
}

export const SESSION_STORE_KEY: InjectionKey<SessionStore> = Symbol('rodaq.session');

const MAX_NOTIFICATIONS = 5;

export function createSessionStore(fetchImpl?: typeof fetch): SessionStore {
  // CSRF token lives strictly in memory, never written to localStorage/sessionStorage.
  let csrfToken: string | null = null;
  let nextNotificationId = 1;

  const state = reactive<{
    lifecycle: AppLifecycleState;
    identity: SessionIdentity | null;
    serverPhase: ServerPhase | null;
    systemSnapshot: SystemData | null;
    capabilities: CapabilitiesData | null;
    lastError: ApiFailure | null;
    incompatibleReason: string | null;
    intendedRoute: string | null;
    notifications: NotificationItem[];
  }>({
    lifecycle: 'Bootstrapping',
    identity: null,
    serverPhase: null,
    systemSnapshot: null,
    capabilities: null,
    lastError: null,
    incompatibleReason: null,
    intendedRoute: null,
    notifications: [],
  });

  function clearMemorySession(): void {
    csrfToken = null;
    state.identity = null;
  }

  function handleUnauthenticated(): void {
    clearMemorySession();
    if (state.lifecycle !== 'ApiIncompatible') {
      state.lifecycle = 'Unauthenticated';
    }
  }

  function handleApiIncompatible(reason: string): void {
    state.incompatibleReason = reason;
    state.lifecycle = 'ApiIncompatible';
  }

  const api = createApiClient(
    {
      getCsrfToken: () => csrfToken,
      onUnauthenticated: handleUnauthenticated,
      onApiIncompatible: handleApiIncompatible,
    },
    fetchImpl,
  );

  function pushNotification(tone: NotificationItem['tone'], message: string): void {
    state.notifications.push({ id: nextNotificationId++, tone, message });
    if (state.notifications.length > MAX_NOTIFICATIONS) {
      state.notifications.splice(0, state.notifications.length - MAX_NOTIFICATIONS);
    }
  }

  function dismissNotification(id: number): void {
    const idx = state.notifications.findIndex((n) => n.id === id);
    if (idx >= 0) {
      state.notifications.splice(idx, 1);
    }
  }

  function applySystemPhase(sys: SystemData): void {
    state.systemSnapshot = sys;
    state.serverPhase = sys.server_phase;
    if (state.lifecycle === 'ApiIncompatible' || state.identity === null) {
      return;
    }
    if (sys.server_phase === 'ready') {
      const wasRecovering = state.lifecycle !== 'AuthenticatedReady';
      state.lifecycle = 'AuthenticatedReady';
      if (wasRecovering && !state.capabilities) {
        void refreshCapabilities();
      }
    } else {
      state.lifecycle = 'AuthenticatedRecovering';
    }
  }

  async function refreshCapabilities(): Promise<void> {
    if (!state.identity || state.lifecycle === 'ApiIncompatible') {
      return;
    }
    try {
      const caps = await api.request<CapabilitiesData>({
        method: 'GET',
        path: '/api/capabilities',
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
      state.capabilities = caps;
    } catch (err) {
      if (err instanceof ApiFailure && err.httpStatus === 503) {
        // Recovering/unavailable: leave capabilities null until Ready.
        return;
      }
      if (err instanceof ApiFailure) {
        state.lastError = err;
      }
    }
  }

  async function loadSystemAfterAuth(): Promise<void> {
    const sys = await api.request<SystemData>({
      method: 'GET',
      path: '/api/system',
      timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
    });
    applySystemPhase(sys);
    if (sys.server_phase === 'ready') {
      await refreshCapabilities();
    }
  }

  async function bootstrap(): Promise<void> {
    state.lifecycle = 'Bootstrapping';
    state.lastError = null;
    try {
      const session = await api.request<SessionData>({
        method: 'GET',
        path: '/api/session',
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
      csrfToken = session.csrf_token;
      state.identity = {
        principal: session.principal,
        scopes: [...session.scopes],
        apiVersion: { ...session.api_version },
      };
      await loadSystemAfterAuth();
    } catch (err) {
      if (err instanceof ApiFailure) {
        state.lastError = err;
        if (err.kind === 'unauthenticated') {
          clearMemorySession();
          state.lifecycle = 'Unauthenticated';
          return;
        }
        if (err.kind === 'api_incompatible') {
          handleApiIncompatible(err.message);
          return;
        }
      }
      state.lifecycle = 'ServerUnavailable';
    }
  }

  async function login(bearerToken: string): Promise<void> {
    state.lifecycle = 'Authenticating';
    state.lastError = null;
    try {
      const session = await api.request<SessionData>({
        method: 'POST',
        path: '/api/session',
        bearerToken: bearerToken.trim(),
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
      csrfToken = session.csrf_token;
      state.identity = {
        principal: session.principal,
        scopes: [...session.scopes],
        apiVersion: { ...session.api_version },
      };
      await loadSystemAfterAuth();
    } catch (err) {
      clearMemorySession();
      if (err instanceof ApiFailure) {
        state.lastError = err;
        if (err.kind === 'api_incompatible') {
          handleApiIncompatible(err.message);
          return;
        }
        state.lifecycle = err.kind === 'network' ? 'ServerUnavailable' : 'Unauthenticated';
        throw err;
      }
      state.lifecycle = 'Unauthenticated';
      throw err;
    }
  }

  async function logout(): Promise<void> {
    try {
      await api.request<void>({
        method: 'DELETE',
        path: '/api/session',
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
    } catch {
      // Idempotent logout: always clear in-memory session even on network failure.
    } finally {
      clearMemorySession();
      state.capabilities = null;
      state.systemSnapshot = null;
      state.serverPhase = null;
      state.intendedRoute = null;
      state.lifecycle = 'Unauthenticated';
    }
  }

  const store: SessionStore = {
    get lifecycle() {
      return state.lifecycle;
    },
    get identity() {
      return state.identity;
    },
    get serverPhase() {
      return state.serverPhase;
    },
    get systemSnapshot() {
      return state.systemSnapshot;
    },
    get capabilities() {
      return state.capabilities;
    },
    get lastError() {
      return state.lastError;
    },
    get incompatibleReason() {
      return state.incompatibleReason;
    },
    get intendedRoute() {
      return state.intendedRoute;
    },
    get notifications() {
      return readonly(state.notifications);
    },
    api,
    get pollingFloorMs() {
      return durationToMsClamped(state.capabilities?.api_limits.recommended_polling_floor, 500);
    },
    get idempotencyRetentionMs() {
      return durationToMsClamped(
        state.capabilities?.api_limits.idempotency_retention,
        24 * 60 * 60 * 1000,
      );
    },
    hasScope(scope: Scope) {
      return state.identity?.scopes.includes(scope) ?? false;
    },
    setIntendedRoute(path: string | null) {
      if (path && (path.startsWith('/login') || path.includes('Bearer'))) {
        state.intendedRoute = null;
      } else {
        state.intendedRoute = path;
      }
    },
    pushNotification,
    dismissNotification,
    bootstrap,
    login,
    logout,
    refreshCapabilities,
    updateServerPhaseFromSystem: applySystemPhase,
  };

  return store;
}

export function provideSessionStore(store: SessionStore): void {
  provide(SESSION_STORE_KEY, store);
}

export function useSessionStore(): SessionStore {
  const store = inject(SESSION_STORE_KEY);
  if (!store) {
    throw new Error('SessionStore not provided');
  }
  return store;
}
