import { reactive } from 'vue'
import { createApiClient, ApiFailure } from '../api/client'
import type { CapabilitiesData, Scope, SessionData, SystemData } from '../api/types'

export type AppLifecycleState =
  | 'Bootstrapping'
  | 'ServerUnavailable'
  | 'Unauthenticated'
  | 'Authenticating'
  | 'AuthenticatedRecovering'
  | 'AuthenticatedReady'
  | 'ApiIncompatible'

export interface SessionStore {
  lifecycle: AppLifecycleState
  session: SessionData | null
  system: SystemData | null
  capabilities: CapabilitiesData | null
  intendedRoute: string | null
  errorMessage: string | null
}

const state = reactive<SessionStore>({
  lifecycle: 'Bootstrapping',
  session: null,
  system: null,
  capabilities: null,
  intendedRoute: null,
  errorMessage: null
})

export const apiClient = createApiClient({
  getCsrfToken: () => state.session?.csrf_token ?? null,
  onUnauthenticated: () => {
    state.session = null
    state.lifecycle = 'Unauthenticated'
  },
  onApiIncompatible: (reason) => {
    state.errorMessage = reason
    state.lifecycle = 'ApiIncompatible'
  }
})

export function useSession() {
  async function refreshSystemAndCapabilities(): Promise<void> {
    const sys = await apiClient.request<SystemData>({
      method: 'GET',
      path: '/api/system',
      timeoutMs: 5000
    })
    state.system = sys
    if (sys.server_phase === 'recovering') {
      state.lifecycle = 'AuthenticatedRecovering'
      return
    }
    state.lifecycle = 'AuthenticatedReady'
    try {
      state.capabilities = await apiClient.request<CapabilitiesData>({
        method: 'GET',
        path: '/api/capabilities',
        timeoutMs: 5000
      })
    } catch {
      // Capabilities can be retried on /system without blocking Ready shell.
    }
  }

  async function bootstrap(): Promise<void> {
    state.lifecycle = 'Bootstrapping'
    state.errorMessage = null
    try {
      const sess = await apiClient.request<SessionData>({
        method: 'GET',
        path: '/api/session',
        timeoutMs: 5000
      })
      state.session = sess
      await refreshSystemAndCapabilities()
    } catch (err) {
      if (err instanceof ApiFailure) {
        if (err.kind === 'unauthenticated') {
          state.session = null
          state.lifecycle = 'Unauthenticated'
          return
        }
        if (err.kind === 'api_incompatible') {
          state.lifecycle = 'ApiIncompatible'
          state.errorMessage = err.message
          return
        }
        state.errorMessage = err.message
      } else {
        state.errorMessage = 'Unable to reach RoDAQ Server'
      }
      state.lifecycle = 'ServerUnavailable'
    }
  }

  async function loginWithBearer(bearerToken: string): Promise<void> {
    state.lifecycle = 'Authenticating'
    state.errorMessage = null
    try {
      const sess = await apiClient.request<SessionData>({
        method: 'POST',
        path: '/api/session',
        bearerToken,
        timeoutMs: 5000
      })
      state.session = sess
      await refreshSystemAndCapabilities()
    } catch (err) {
      state.session = null
      state.lifecycle = 'Unauthenticated'
      state.errorMessage = err instanceof Error ? err.message : 'Authentication failed'
      throw err
    }
  }

  async function logout(): Promise<void> {
    try {
      await apiClient.request<void>({ method: 'DELETE', path: '/api/session', timeoutMs: 5000 })
    } catch {
      // Best-effort server logout; always clear local memory state.
    }
    state.session = null
    state.system = null
    state.capabilities = null
    state.lifecycle = 'Unauthenticated'
  }

  function hasScope(scope: Scope): boolean {
    return state.session?.scopes.includes(scope) ?? false
  }

  return {
    state,
    bootstrap,
    loginWithBearer,
    logout,
    refreshSystemAndCapabilities,
    hasScope
  }
}
