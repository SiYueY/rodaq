import { ref } from 'vue';
import type { ArtifactFileData } from '../../api/generated';
import { ApiFailure } from '../../api/errors';
import { useSessionStore } from '../../composables/useSession';

/**
 * Artifact download helper (Web §2.6, WEB-E2E-009):
 * 1. Validates same-origin relative `/api/artifacts/...` download_url (never filesystem paths).
 * 2. Runs authenticated HEAD preflight to surface common auth/committed/lease errors in-SPA.
 * 3. Triggers native browser navigation download via anchor element — never buffers file in JS Blob/memory.
 */
export function useArtifactDownload() {
  const session = useSessionStore();
  const busyFileId = ref<string | null>(null);
  const error = ref<ApiFailure | null>(null);
  const notice = ref<string | null>(null);

  let activeController: AbortController | null = null;

  function abortPreflight(): void {
    if (activeController) {
      activeController.abort();
      activeController = null;
    }
  }

  async function downloadFile(
    file: ArtifactFileData,
    triggerNavigation?: (url: string) => void,
  ): Promise<boolean> {
    if (!file.download_url.startsWith('/api/artifacts/')) {
      error.value = new ApiFailure({
        message: 'Refused non-API or cross-origin download URL',
        httpStatus: null,
        requestId: null,
        retryAfterMs: null,
        apiError: null,
        kind: 'validation',
      });
      return false;
    }

    abortPreflight();
    const controller = new AbortController();
    activeController = controller;
    busyFileId.value = file.file_id;
    error.value = null;
    notice.value = null;

    try {
      await session.api.head(file.download_url, controller.signal);
      if (triggerNavigation) {
        triggerNavigation(file.download_url);
      } else {
        const anchor = document.createElement('a');
        anchor.href = file.download_url;
        anchor.download = file.display_name;
        anchor.rel = 'noopener';
        document.body.appendChild(anchor);
        anchor.click();
        document.body.removeChild(anchor);
      }
      notice.value =
        'Browser download started. Note: if concurrent Retention/Reset occurs before the server acquires a ReadLease, the final browser GET may fail — refresh artifact status and retry.';
      return true;
    } catch (err) {
      if (controller.signal.aborted) {
        return false;
      }
      error.value =
        err instanceof ApiFailure
          ? err
          : new ApiFailure({
              message: err instanceof Error ? err.message : 'Download preflight failed',
              httpStatus: null,
              requestId: null,
              retryAfterMs: null,
              apiError: null,
              kind: 'network',
            });
      return false;
    } finally {
      if (activeController === controller) {
        activeController = null;
      }
      busyFileId.value = null;
    }
  }

  return {
    busyFileId,
    error,
    notice,
    downloadFile,
    abortPreflight,
  };
}
