import type { DurationValue, Rfc3339Timestamp } from '../api/generated';
import { parseUint64BigInt } from './decimal';

const UNIT_NS: Record<DurationValue['unit'], bigint> = {
  ns: 1n,
  us: 1_000n,
  ms: 1_000_000n,
  s: 1_000_000_000n,
};

export function durationToNanoseconds(d: DurationValue | null | undefined): bigint | null {
  if (!d) {
    return null;
  }
  const v = parseUint64BigInt(d.value);
  if (v === null) {
    return null;
  }
  const mult = UNIT_NS[d.unit];
  if (!mult) {
    return null;
  }
  return v * mult;
}

export function durationToMsClamped(d: DurationValue | null | undefined, fallbackMs: number): number {
  const ns = durationToNanoseconds(d);
  if (ns === null) {
    return fallbackMs;
  }
  const ms = ns / 1_000_000n;
  if (ms > BigInt(Number.MAX_SAFE_INTEGER)) {
    return Number.MAX_SAFE_INTEGER;
  }
  return Number(ms);
}

export function formatDuration(d: DurationValue | null | undefined): string {
  const ns = durationToNanoseconds(d);
  if (ns === null) {
    return '—';
  }
  if (ns === 0n) {
    return '0 ms';
  }
  if (ns < 1_000n) {
    return `${ns.toString()} ns`;
  }
  if (ns < 1_000_000n) {
    return `${(ns / 1_000n).toString()} µs`;
  }
  if (ns < 1_000_000_000n) {
    return `${(ns / 1_000_000n).toString()} ms`;
  }
  const wholeSec = ns / 1_000_000_000n;
  const remMs = (ns % 1_000_000_000n) / 10_000_000n;
  if (wholeSec < 60n) {
    return `${wholeSec.toString()}.${remMs.toString().padStart(2, '0')} s`;
  }
  const mins = wholeSec / 60n;
  const secs = wholeSec % 60n;
  return `${mins.toString()}m ${secs.toString()}s`;
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
