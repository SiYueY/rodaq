<script setup lang="ts">
import { ref } from 'vue';
import { ApiFailure } from '../../api/errors';
import { useSessionStore } from '../../composables/useSession';
import ErrorPanel from '../../components/ErrorPanel.vue';

const session = useSessionStore();

const bearerInput = ref('');
const submitError = ref<ApiFailure | null>(null);

async function onSubmit(): Promise<void> {
  const token = bearerInput.value.trim();
  if (!token) {
    submitError.value = new ApiFailure({
      message: 'Please enter a local RoDAQ Bearer token.',
      httpStatus: null,
      requestId: null,
      retryAfterMs: null,
      apiError: null,
      kind: 'validation',
    });
    return;
  }
  submitError.value = null;
  try {
    await session.login(token);
    bearerInput.value = '';
  } catch (err) {
    submitError.value =
      err instanceof ApiFailure
        ? err
        : new ApiFailure({
            message: err instanceof Error ? err.message : 'Sign-in failed',
            httpStatus: null,
            requestId: null,
            retryAfterMs: null,
            apiError: null,
            kind: 'network',
          });
  }
}
</script>

<template>
  <main style="max-width: 480px; margin: 64px auto; padding: 0 16px" tabindex="-1">
    <section class="card">
      <h1 style="margin: 0 0 8px 0; font-size: 1.375rem">RoDAQ Local Console Sign In</h1>
      <p class="muted" style="margin: 0 0 16px 0; font-size: 0.875rem">
        Authenticate using the local high-entropy Bearer token. The token is kept strictly in page
        memory (never written to cookies, localStorage, or sessionStorage); refreshing the page
        requires re-entering the token.
      </p>

      <ErrorPanel v-if="submitError" :error="submitError" title="Authentication Failed" />

      <form @submit.prevent="onSubmit">
        <div class="form-field">
          <label for="bearer-token-input">Bearer Token</label>
          <input
            id="bearer-token-input"
            v-model="bearerInput"
            type="password"
            autocomplete="off"
            spellcheck="false"
            placeholder="Paste local bearer token"
            :disabled="session.lifecycle === 'Authenticating'"
            required
          />
        </div>
        <button
          type="submit"
          class="btn btn-primary"
          :disabled="session.lifecycle === 'Authenticating'"
          style="width: 100%"
        >
          {{ session.lifecycle === 'Authenticating' ? 'Signing In…' : 'Sign In' }}
        </button>
      </form>
    </section>
  </main>
</template>
