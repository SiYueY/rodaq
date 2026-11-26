<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router';
import { useSessionStore } from './composables/useSession';
import {
  clearForeignOrCorruptPendingMutations,
  evaluatePendingMutations,
  type PendingMutationDecision,
} from './features/tasks/pendingMutations';
import { useTaskMutation } from './features/tasks/useTaskMutation';
import StatusBadge from './components/StatusBadge.vue';
import ConfirmDialog from './components/ConfirmDialog.vue';

const session = useSessionStore();
const route = useRoute();
const router = useRouter();
const taskMutation = useTaskMutation();

const liveAnnouncement = ref('');
const pendingDecisions = ref<PendingMutationDecision[]>([]);
const showClearForeignModal = ref(false);

const foreignPendingCount = computed(
  () =>
    pendingDecisions.value.filter(
      (d) =>
        d.action === 'blocked_foreign_principal' ||
        d.action === 'blocked_corrupt_or_missing_principal',
    ).length,
);

const expiredOrScopeBlockedCount = computed(
  () =>
    pendingDecisions.value.filter(
      (d) => d.action === 'blocked_expired_ttl' || d.action === 'blocked_insufficient_scope',
    ).length,
);

const routeBlockedByScope = computed(() => {
  if (!session.identity) {
    return false;
  }
  const reqScope = route.meta.requiredScope;
  if (!reqScope) {
    return false;
  }
  return !session.hasScope(reqScope);
});

const routeBlockedByRecovery = computed(() => {
  if (session.lifecycle !== 'AuthenticatedRecovering') {
    return false;
  }
  return route.meta.allowedDuringRecovery === false;
});

async function reconcilePendingMutations(): Promise<void> {
  const decisions = evaluatePendingMutations({
    currentPrincipal: session.identity?.principal ?? null,
    currentScopes: session.identity?.scopes ?? [],
    serverReady: session.lifecycle === 'AuthenticatedReady',
    apiCompatible: session.lifecycle !== 'ApiIncompatible',
    idempotencyRetentionMs: session.idempotencyRetentionMs,
  });
  pendingDecisions.value = decisions;

  for (const d of decisions) {
    if (d.action === 'eligible_replay' || d.action === 'eligible_poll_operation') {
      await taskMutation.replayPendingMutation(d.record);
    }
  }
  pendingDecisions.value = evaluatePendingMutations({
    currentPrincipal: session.identity?.principal ?? null,
    currentScopes: session.identity?.scopes ?? [],
    serverReady: session.lifecycle === 'AuthenticatedReady',
    apiCompatible: session.lifecycle !== 'ApiIncompatible',
    idempotencyRetentionMs: session.idempotencyRetentionMs,
  });
}

function confirmClearForeignPending(): void {
  if (session.identity) {
    clearForeignOrCorruptPendingMutations(session.identity.principal);
    liveAnnouncement.value = 'Cleared unsettled local requests belonging to other principals.';
  }
  showClearForeignModal.value = false;
  void reconcilePendingMutations();
}

let signingOut = false;

async function handleLogout(): Promise<void> {
  signingOut = true;
  try {
    await session.logout();
    session.setIntendedRoute(null);
    liveAnnouncement.value = 'Signed out.';
    await router.replace({ name: 'login' });
  } finally {
    signingOut = false;
  }
}

watch(
  () => session.lifecycle,
  async (next, prev) => {
    if (next === 'Unauthenticated' && route.name !== 'login') {
      if (signingOut) {
        return;
      }
      session.setIntendedRoute(route.fullPath);
      liveAnnouncement.value = 'Session expired. Please sign in.';
      const query = route.fullPath && route.fullPath !== '/' ? { next: route.fullPath } : undefined;
      await router.replace({ name: 'login', query });
    } else if (next === 'AuthenticatedReady') {
      if (prev === 'AuthenticatedRecovering') {
        liveAnnouncement.value = 'Server is now ready.';
      }
      await reconcilePendingMutations();
      if (route.name === 'login') {
        const nextQ = typeof route.query.next === 'string' ? route.query.next : null;
        const safeNext = nextQ && nextQ.startsWith('/') && !nextQ.startsWith('//') ? nextQ : null;
        const target = safeNext ?? session.intendedRoute ?? '/';
        session.setIntendedRoute(null);
        await router.replace(target);
      }
    } else if (next === 'AuthenticatedRecovering') {
      liveAnnouncement.value = 'Server is recovering Core authority.';
      if (route.name === 'login') {
        await router.replace('/');
      }
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
      <p class="muted">Checking local session and server status…</p>
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
        <div>
          Principal: <strong>{{ session.identity?.principal ?? '—' }}</strong>
        </div>
        <div>
          Scopes: {{ session.identity?.scopes.join(', ') ?? '—' }}
        </div>
        <div>
          API: v{{ session.identity?.apiVersion.major }}.{{ session.identity?.apiVersion.minor }}
        </div>
        <button type="button" class="btn" @click="handleLogout">Sign Out</button>
      </div>
    </aside>

    <main class="app-main" tabindex="-1">
      <div v-if="session.lifecycle === 'AuthenticatedRecovering'" class="banner banner-warning" role="status">
        <span>Server is in <strong>Recovering</strong> limited mode. Mutations and downloads are disabled until Core recovery completes.</span>
        <button type="button" class="btn" @click="session.bootstrap()">Re-check Status</button>
      </div>

      <div v-if="foreignPendingCount > 0" class="banner banner-info" role="status">
        <span>
          {{ foreignPendingCount }} unsettled local request(s) belong to another principal or an invalid record. Details are hidden and will not be replayed automatically.
        </span>
        <button type="button" class="btn" @click="showClearForeignModal = true">
          Clear Local Records
        </button>
      </div>

      <div v-if="expiredOrScopeBlockedCount > 0" class="banner banner-warning" role="status">
        <span>
          {{ expiredOrScopeBlockedCount }} local pending request(s) could not be automatically recovered (expired retention window or insufficient scope). Please verify Task and Artifact facts manually.
        </span>
      </div>

      <div v-for="n in session.notifications" :key="n.id" class="banner" :class="`banner-${n.tone}`">
        <span>{{ n.message }}</span>
        <button type="button" class="btn" @click="session.dismissNotification(n.id)">Dismiss</button>
      </div>

      <section v-if="routeBlockedByScope" class="card" role="alert">
        <h1>Permission Denied (403)</h1>
        <p>
          Current principal <strong>{{ session.identity?.principal }}</strong> lacks required scope
          <code>{{ route.meta.requiredScope }}</code> for this page.
        </p>
      </section>

      <section v-else-if="routeBlockedByRecovery" class="card" role="status">
        <h1>Waiting for Core Recovery</h1>
        <p>
          This route requires the server to finish recovery. Your URL has been preserved and will activate automatically once the server becomes Ready.
        </p>
        <div class="row-actions">
          <RouterLink class="btn" to="/">Go to Overview</RouterLink>
          <RouterLink class="btn" to="/system">Inspect System Recovery</RouterLink>
        </div>
      </section>

      <RouterView v-else />
    </main>
  </div>

  <ConfirmDialog
    :open="showClearForeignModal"
    title="Clear Unsettled Local Request Records"
    confirm-label="Clear Local Records"
    :danger="true"
    @cancel="showClearForeignModal = false"
    @confirm="confirmClearForeignPending"
  >
    <p>
      This removes unsettled local browser records created by other principals from this tab's
      <code>sessionStorage</code>. It does not cancel or modify any operation on the server.
    </p>
  </ConfirmDialog>
</template>
