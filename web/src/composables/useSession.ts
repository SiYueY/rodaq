import { inject, provide, reactive, readonly, type InjectionKey } from 'vue';
import { CLIENT_TIMEOUTS, createApiClient, type ApiClient } from '../api/client';
import type { CapabilitiesData, ServerPhase, SystemData } from '../api/generated';
import type { AppLifecycleState } from '../api/types';
import { ApiFailure } from '../api/errors';

export interface NotificationItem {
  id: number;
  tone: 'info' | 'warning' | 'danger' | 'success';
  message: string;
}

export interface SessionStore {
  readonly lifecycle: AppLifecycleState;
  readonly authenticated: boolean;
  readonly serverPhase: ServerPhase | null;
  readonly systemSnapshot: SystemData | null;
  readonly capabilities: CapabilitiesData | null;
  readonly lastError: ApiFailure | null;
  readonly incompatibleReason: string | null;
  readonly intendedRoute: string | null;
  readonly notifications: readonly NotificationItem[];
  readonly api: ApiClient;
  readonly pollingFloorMs: number;
  setIntendedRoute(path: string | null): void;
  pushNotification(tone: NotificationItem['tone'], message: string): void;
  dismissNotification(id: number): void;
  bootstrap(): Promise<void>;
  login(bearerToken: string): Promise<void>;
  logout(): void;
  refreshCapabilities(): Promise<void>;
  updateServerPhaseFromSystem(sys: SystemData): void;
}

export const SESSION_STORE_KEY: InjectionKey<SessionStore> = Symbol('rodaq.session');

const MAX_NOTIFICATIONS = 5;

export function createSessionStore(fetchImpl?: typeof fetch): SessionStore {
  // Single high-entropy Bearer token stored in memory ONLY (never cookie/localStorage/sessionStorage).
  let bearerToken: string | null = null;
  let nextNotificationId = 1;

  const state = reactive<{
    lifecycle: AppLifecycleState;
    authenticated: boolean;
    serverPhase: ServerPhase | null;
    systemSnapshot: SystemData | null;
    capabilities: CapabilitiesData | null;
    lastError: ApiFailure | null;
    incompatibleReason: string | null;
    intendedRoute: string | null;
    notifications: NotificationItem[];
  }>({
    lifecycle: 'Unauthenticated',
    authenticated: false,
    serverPhase: null,
    systemSnapshot: null,
    capabilities: null,
    lastError: null,
    incompatibleReason: null,
    intendedRoute: null,
    notifications: [],
  });

  function clearMemoryAuth(): void {
    bearerToken = null;
    state.authenticated = false;
  }

  function handleUnauthenticated(): void {
    clearMemoryAuth();
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
      getBearerToken: () => bearerToken,
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
    if (state.lifecycle === 'ApiIncompatible' || !state.authenticated) {
      return;
    }
    state.lifecycle = 'AuthenticatedReady';
  }

  async function refreshCapabilities(): Promise<void> {
    if (!state.authenticated || state.lifecycle === 'ApiIncompatible') {
      return;
    }
    try {
      state.capabilities = await api.request<CapabilitiesData>({
        method: 'GET',
        path: '/api/capabilities',
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
    } catch (err) {
      if (err instanceof ApiFailure) {
        state.lastError = err;
      }
    }
  }

  async function bootstrap(): Promise<void> {
    state.lastError = null;
    if (!bearerToken) {
      state.lifecycle = 'Unauthenticated';
      return;
    }
    state.lifecycle = 'Bootstrapping';
    try {
      const sys = await api.request<SystemData>({
        method: 'GET',
        path: '/api/system',
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
      state.authenticated = true;
      applySystemPhase(sys);
      await refreshCapabilities();
    } catch (err) {
      if (err instanceof ApiFailure) {
        state.lastError = err;
        if (err.kind === 'unauthenticated') {
          clearMemoryAuth();
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

  async function login(rawToken: string): Promise<void> {
    const trimmed = rawToken.trim();
    state.lifecycle = 'Authenticating';
    state.lastError = null;
    try {
      const sys = await api.request<SystemData>({
        method: 'GET',
        path: '/api/system',
        bearerToken: trimmed,
        suppressAuthHook: true,
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      });
      bearerToken = trimmed;
      state.authenticated = true;
      applySystemPhase(sys);
      await refreshCapabilities();
    } catch (err) {
      clearMemoryAuth();
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

  function logout(): void {
    clearMemoryAuth();
    state.capabilities = null;
    state.systemSnapshot = null;
    state.serverPhase = null;
    state.intendedRoute = null;
    state.lifecycle = 'Unauthenticated';
  }

  const store: SessionStore = {
    get lifecycle() {
      return state.lifecycle;
    },
    get authenticated() {
      return state.authenticated;
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
      return state.capabilities?.api_limits.recommended_polling_floor_ms ?? 500;
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
