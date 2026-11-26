import { createApp } from 'vue';
import './styles/tokens.css';
import './styles/app.css';
import App from './App.vue';
import { createSessionStore, SESSION_STORE_KEY } from './composables/useSession';
import { createAppRouter } from './router';

const session = createSessionStore();
const router = createAppRouter(session);

const app = createApp(App);
app.provide(SESSION_STORE_KEY, session);
app.use(router);
app.mount('#app');
