import type { Rfc3339Timestamp } from '../api/generated';

export function formatDurationMs(ms: number | null | undefined): string {
  if (ms === null || ms === undefined || !Number.isFinite(ms) || ms < 0) {
    return '—';
  }
  const whole = Math.floor(ms);
  if (whole === 0) {
    return '0 ms';
  }
  if (whole < 1_000) {
    return `${whole} ms`;
  }
  const totalSec = Math.floor(whole / 1_000);
  const remTenths = Math.floor((whole % 1_000) / 10);
  if (totalSec < 60) {
    return `${totalSec}.${remTenths.toString().padStart(2, '0')} s`;
  }
  const mins = Math.floor(totalSec / 60);
  const secs = totalSec % 60;
  return `${mins}m ${secs}s`;
}

export function formatTimestampUtc(ts: Rfc3339Timestamp | null | undefined): string {
  if (!ts) {
    return '—';
  }
  const parsed = Date.parse(ts);
  if (Number.isNaN(parsed)) {
    return ts;
  }
  return new Date(parsed).toISOString().replace('T', ' ').replace(/\.\d+Z$/, 'Z');
}

export function formatAgeSeconds(nowMs: number, receivedAtMs: number | null): string {
  if (receivedAtMs === null) {
    return '—';
  }
  const diffSec = Math.max(0, Math.floor((nowMs - receivedAtMs) / 1000));
  if (diffSec < 60) {
    return `${diffSec}s ago`;
  }
  const mins = Math.floor(diffSec / 60);
  const secs = diffSec % 60;
  return `${mins}m ${secs}s ago`;
}
