<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { useSession } from '../../composables/useSession'

const router = useRouter()
const { state, loginWithBearer } = useSession()
const bearer = ref('')
const busy = ref(false)
const localError = ref<string | null>(null)

async function submit() {
  if (!bearer.value.trim()) {
    localError.value = 'Bearer credential is required'
    return
  }
  busy.value = true
  localError.value = null
  try {
    await loginWithBearer(bearer.value.trim())
    bearer.value = ''
    const next = state.intendedRoute || '/'
    state.intendedRoute = null
    await router.replace(next)
  } catch (err) {
    localError.value = err instanceof Error ? err.message : 'Authentication failed'
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <main class="main" style="max-width: 460px; margin-top: 64px">
    <div class="card">
      <h1>RoDAQ Local Console Sign In</h1>
      <p style="color: var(--muted)">
        Authenticate with your local Bearer credential. Credentials are never stored in browser
        storage; an HttpOnly session cookie and in-memory CSRF token are established.
      </p>
      <div v-if="localError || state.errorMessage" class="error-box" role="alert">
        {{ localError || state.errorMessage }}
      </div>
      <form @submit.prevent="submit">
        <label for="bearer-input" style="display: block; margin-bottom: 6px">Bearer Credential</label>
        <input
          id="bearer-input"
          v-model="bearer"
          type="password"
          autocomplete="off"
          style="width: 100%; margin-bottom: 14px"
          placeholder="Enter provisioned bearer token"
        />
        <button class="btn btn-primary" type="submit" :disabled="busy" style="width: 100%">
          {{ busy ? 'Authenticating…' : 'Sign In' }}
        </button>
      </form>
    </div>
  </main>
</template>
