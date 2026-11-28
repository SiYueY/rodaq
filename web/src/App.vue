<script setup lang="ts">
import { onMounted, ref, watch } from 'vue';
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router';
import { useSessionStore } from './composables/useSession';
import StatusBadge from './components/StatusBadge.vue';

const session = useSessionStore();
const route = useRoute();
const router = useRouter();

const liveAnnouncement = ref('');
let signingOut = false;

async function handleLogout(): Promise<void> {
  signingOut = true;
  try {
    session.logout();
    session.setIntendedRoute(null);
    liveAnnouncement.value = 'Signed out.';
    await router.replace({ name: 'login' });
  } finally {
    signingOut = false;
  }
}

watch(
  () => session.lifecycle,
  async (next) => {
    if (next === 'Unauthenticated' && route.name !== 'login') {
      if (signingOut) {
        return;
      }
      session.setIntendedRoute(route.fullPath);
      liveAnnouncement.value = 'Authentication required. Please sign in.';
      const query = route.fullPath && route.fullPath !== '/' ? { next: route.fullPath } : undefined;
      await router.replace({ name: 'login', query });
    } else if (next === 'AuthenticatedReady' && route.name === 'login') {
      const nextQ = typeof route.query.next === 'string' ? route.query.next : null;
      const safeNext = nextQ && nextQ.startsWith('/') && !nextQ.startsWith('//') ? nextQ : null;
      const target = safeNext ?? session.intendedRoute ?? '/';
      session.setIntendedRoute(null);
      await router.replace(target);
    }
  },
);

onMounted(() => {
  void session.bootstrap();
});
</script>

<template>
  <div aria-live="polite" class="sr-only">{{ liveAnnouncement }}</div>

  <div v-if="session.lifecycle === 'Bootstrapping'" style="padding: 48px; max-width: 560px; margin: 0 auto">
    <section class="card">
      <h1>RoDAQ Console</h1>
      <p class="muted">Connecting to local RoDAQ server…</p>
      <div class="skeleton" aria-hidden="true" />
    </section>
  </div>

  <div
    v-else-if="session.lifecycle === 'ApiIncompatible'"
    style="padding: 48px; max-width: 640px; margin: 0 auto"
  >
    <section class="card" role="alert">
      <h1>Incompatible API Version</h1>
      <p>{{ session.incompatibleReason ?? 'Web console API major version does not match Server.' }}</p>
      <p class="muted">All polling and control mutations are halted. Deploy matching Server and Web bundles.</p>
      <button type="button" class="btn btn-primary" @click="session.bootstrap()">Retry Probe</button>
    </section>
  </div>

  <div
    v-else-if="session.lifecycle === 'ServerUnavailable'"
    style="padding: 48px; max-width: 560px; margin: 0 auto"
  >
    <section class="card" role="alert">
      <h1>Server Unavailable</h1>
      <p>{{ session.lastError?.message ?? 'Unable to reach local RoDAQ Server.' }}</p>
      <button type="button" class="btn btn-primary" @click="session.bootstrap()">Retry Connection</button>
    </section>
  </div>

  <div
    v-else-if="session.lifecycle === 'Unauthenticated' || session.lifecycle === 'Authenticating'"
  >
    <RouterView />
  </div>

  <div v-else class="app-shell">
    <aside class="app-sidebar" aria-label="Primary navigation">
      <div class="app-brand">
        <span>RoDAQ</span>
        <StatusBadge
          :label="session.serverPhase ?? 'unknown'"
          :tone="session.serverPhase === 'ready' ? 'success' : 'warning'"
        />
      </div>
      <nav class="app-nav">
        <RouterLink to="/">Overview</RouterLink>
        <RouterLink to="/tasks">Tasks</RouterLink>
        <RouterLink to="/artifacts">Artifacts</RouterLink>
        <RouterLink to="/system">System</RouterLink>
      </nav>
      <div class="app-sidebar-footer">
        <div class="muted">Auth: <strong>Local Bearer (memory)</strong></div>
        <div class="muted">API: v1.0</div>
        <button type="button" class="btn" @click="handleLogout">Sign Out</button>
      </div>
    </aside>

    <main class="app-main" tabindex="-1">
      <div v-for="n in session.notifications" :key="n.id" class="banner" :class="`banner-${n.tone}`">
        <span>{{ n.message }}</span>
        <button type="button" class="btn" @click="session.dismissNotification(n.id)">Dismiss</button>
      </div>
      <RouterView />
    </main>
  </div>
</template>
