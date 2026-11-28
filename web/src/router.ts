import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router';
import type { SessionStore } from './composables/useSession';
import LoginPage from './features/session/LoginPage.vue';
import OverviewPage from './features/overview/OverviewPage.vue';
import TaskListPage from './features/tasks/TaskListPage.vue';
import TaskEditorPage from './features/tasks/TaskEditorPage.vue';
import TaskDetailPage from './features/tasks/TaskDetailPage.vue';
import ArtifactsPage from './features/artifacts/ArtifactsPage.vue';
import SystemPage from './features/system/SystemPage.vue';

declare module 'vue-router' {
  interface RouteMeta {
    requiresAuth?: boolean;
    title?: string;
  }
}

export const routes: RouteRecordRaw[] = [
  {
    path: '/login',
    name: 'login',
    component: LoginPage,
    meta: { requiresAuth: false, title: 'Sign In' },
  },
  {
    path: '/',
    name: 'overview',
    component: OverviewPage,
    meta: { requiresAuth: true, title: 'Overview' },
  },
  {
    path: '/tasks',
    name: 'tasks',
    component: TaskListPage,
    meta: { requiresAuth: true, title: 'Tasks' },
  },
  {
    path: '/tasks/new',
    name: 'task-create',
    component: TaskEditorPage,
    meta: { requiresAuth: true, title: 'Create Task' },
  },
  {
    path: '/tasks/:taskId',
    name: 'task-detail',
    component: TaskDetailPage,
    meta: { requiresAuth: true, title: 'Task Detail' },
  },
  {
    path: '/tasks/:taskId/config',
    name: 'task-configure',
    component: TaskEditorPage,
    meta: { requiresAuth: true, title: 'Configure Task' },
  },
  {
    path: '/artifacts',
    name: 'artifacts',
    component: ArtifactsPage,
    meta: { requiresAuth: true, title: 'Artifacts' },
  },
  {
    path: '/system',
    name: 'system',
    component: SystemPage,
    meta: { requiresAuth: true, title: 'System' },
  },
  {
    path: '/:pathMatch(.*)*',
    name: 'not-found',
    component: {
      template:
        '<section class="card"><h1>Page Not Found</h1><p class="muted">The requested console route does not exist.</p></section>',
    },
    meta: { requiresAuth: true, title: 'Not Found' },
  },
];

export function createAppRouter(session: SessionStore) {
  const router = createRouter({
    history: createWebHistory(),
    routes,
  });

  router.beforeEach((to) => {
    if (session.lifecycle === 'Bootstrapping' || session.lifecycle === 'ApiIncompatible') {
      return true;
    }
    const requiresAuth = to.meta.requiresAuth !== false;
    if (!requiresAuth) {
      if (session.lifecycle === 'AuthenticatedReady' && to.name === 'login') {
        return { path: session.intendedRoute ?? '/' };
      }
      return true;
    }
    if (session.lifecycle === 'Unauthenticated' || session.lifecycle === 'Authenticating') {
      session.setIntendedRoute(to.fullPath);
      const query = to.fullPath && to.fullPath !== '/' ? { next: to.fullPath } : undefined;
      return { name: 'login', query };
    }
    return true;
  });

  router.afterEach(() => {
    queueMicrotask(() => {
      const heading = document.querySelector<HTMLElement>('main h1, main[tabindex]');
      if (heading) {
        if (!heading.hasAttribute('tabindex')) {
          heading.setAttribute('tabindex', '-1');
        }
        heading.focus();
      }
    });
  });

  return router;
}
