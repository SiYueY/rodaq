<script setup lang="ts">
import { onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useSession } from './composables/useSession'

const router = useRouter()
const { state, bootstrap, logout } = useSession()

async function handleLogout() {
  await logout()
  await router.replace('/login')
}

onMounted(() => {
  void bootstrap()
})
</script>

<template>
  <div class="shell">
    <header class="topbar">
      <div class="row">
        <span class="brand">RoDAQ V1</span>
        <nav v-if="state.session" class="nav" aria-label="Main navigation">
          <router-link to="/">Overview</router-link>
          <router-link to="/tasks">Tasks</router-link>
          <router-link to="/artifacts">Artifacts</router-link>
          <router-link to="/system">System</router-link>
        </nav>
      </div>
      <div v-if="state.session" class="row">
        <span class="mono" style="color: var(--muted)">
          {{ state.session.principal }} ({{ state.session.scopes.join(', ') }})
        </span>
        <button class="btn" @click="handleLogout">Sign Out</button>
      </div>
    </header>

    <div v-if="state.lifecycle === 'Bootstrapping'" class="main">
      <div class="card">Connecting to RoDAQ Server…</div>
    </div>

    <div v-else-if="state.lifecycle === 'ApiIncompatible'" class="main">
      <div class="card error-box" role="alert">
        <h2>API Version Incompatible</h2>
        <p>{{ state.errorMessage }}</p>
        <button class="btn" @click="bootstrap">Retry Version Check</button>
      </div>
    </div>

    <div v-else-if="state.lifecycle === 'ServerUnavailable'" class="main">
      <div class="card error-box" role="alert">
        <h2>RoDAQ Server Unavailable</h2>
        <p>{{ state.errorMessage }}</p>
        <button class="btn btn-primary" @click="bootstrap">Retry Connection</button>
      </div>
    </div>

    <router-view v-else />
  </div>
</template>
