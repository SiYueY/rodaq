import { createRouter, createWebHistory } from 'vue-router'
import { useSession } from './composables/useSession'
import type { Scope } from './api/types'

import LoginPage from './features/session/LoginPage.vue'
import OverviewPage from './features/overview/OverviewPage.vue'
import TaskListPage from './features/tasks/TaskListPage.vue'
import TaskEditorPage from './features/tasks/TaskEditorPage.vue'
import TaskDetailPage from './features/tasks/TaskDetailPage.vue'
import ArtifactsPage from './features/artifacts/ArtifactsPage.vue'
import SystemPage from './features/system/SystemPage.vue'

declare module 'vue-router' {
  interface RouteMeta {
    requiresAuth?: boolean
    requiredScope?: Scope
    allowedDuringRecovery?: boolean
  }
}

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/login', component: LoginPage, meta: { requiresAuth: false } },
    { path: '/', component: OverviewPage, meta: { requiresAuth: true, requiredScope: 'read', allowedDuringRecovery: true } },
    { path: '/tasks', component: TaskListPage, meta: { requiresAuth: true, requiredScope: 'read' } },
    { path: '/tasks/new', component: TaskEditorPage, meta: { requiresAuth: true, requiredScope: 'control' } },
    { path: '/tasks/:taskId', component: TaskDetailPage, meta: { requiresAuth: true, requiredScope: 'read' } },
    { path: '/tasks/:taskId/config', component: TaskEditorPage, meta: { requiresAuth: true, requiredScope: 'control' } },
    { path: '/artifacts', component: ArtifactsPage, meta: { requiresAuth: true, requiredScope: 'read' } },
    { path: '/system', component: SystemPage, meta: { requiresAuth: true, requiredScope: 'read', allowedDuringRecovery: true } },
    { path: '/:pathMatch(.*)*', redirect: '/' }
  ]
})

router.beforeEach((to) => {
  const { state, hasScope } = useSession()
  if (to.meta.requiresAuth === false) {
    return true
  }
  if (state.lifecycle === 'Unauthenticated' || !state.session) {
    state.intendedRoute = to.fullPath
    return '/login'
  }
  if (to.meta.requiredScope && !hasScope(to.meta.requiredScope)) {
    return '/'
  }
  if (state.lifecycle === 'AuthenticatedRecovering' && !to.meta.allowedDuringRecovery) {
    return '/'
  }
  return true
})
