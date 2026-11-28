import { ref } from 'vue';
import type { ArtifactFileData } from '../../api/generated';
import { ApiFailure } from '../../api/errors';
import { useSessionStore } from '../../composables/useSession';

/**
 * Artifact download helper (Web §2.6, §3.3):
 * 1. Validates same-origin relative `/api/artifacts/...` download_url.
 * 2. Runs authenticated HEAD preflight with memory-only Bearer token.
 * 3. Fetches file with explicit Bearer header and triggers browser save.
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
    triggerDownload?: (url: string, blob?: Blob) => void,
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
      if (triggerDownload) {
        triggerDownload(file.download_url);
      } else {
        const { blob } = await session.api.downloadBlob(file.download_url, controller.signal);
        const objectUrl = URL.createObjectURL(blob);
        try {
          const anchor = document.createElement('a');
          anchor.href = objectUrl;
          anchor.download = file.display_name;
          anchor.rel = 'noopener';
          document.body.appendChild(anchor);
          anchor.click();
          document.body.removeChild(anchor);
        } finally {
          URL.revokeObjectURL(objectUrl);
        }
      }
      notice.value = 'Download completed.';
      return true;
    } catch (err) {
      if (controller.signal.aborted) {
        return false;
      }
      error.value =
        err instanceof ApiFailure
          ? err
          : new ApiFailure({
              message: err instanceof Error ? err.message : 'Download failed',
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
