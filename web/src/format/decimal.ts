import type { DecimalString } from '../api/generated';

const DECIMAL_RE = /^(?:0|[1-9][0-9]{0,19})$/;
const UINT64_MAX = 18446744073709551615n;

export function isValidUint64Decimal(raw: string): boolean {
  if (!DECIMAL_RE.test(raw)) {
    return false;
  }
  try {
    const parsed = BigInt(raw);
    return parsed >= 0n && parsed <= UINT64_MAX;
  } catch {
    return false;
  }
}

export function isPositiveUint64Decimal(raw: string): boolean {
  if (!isValidUint64Decimal(raw)) {
    return false;
  }
  return BigInt(raw) > 0n;
}

export function parseUint64BigInt(raw: DecimalString | number | null | undefined): bigint | null {
  if (raw === null || raw === undefined) {
    return null;
  }
  if (typeof raw === 'number') {
    if (!Number.isFinite(raw) || raw < 0 || !Number.isInteger(raw)) {
      return null;
    }
    return BigInt(raw);
  }
  if (!isValidUint64Decimal(raw)) {
    return null;
  }
  return BigInt(raw);
}

export function formatDecimalCount(raw: DecimalString | number | null | undefined): string {
  const val = parseUint64BigInt(raw);
  if (val === null) {
    return '—';
  }
  return val.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

export function formatIecBytes(raw: DecimalString | number | null | undefined): string {
  const bytes = parseUint64BigInt(raw);
  if (bytes === null) {
    return '—';
  }
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB'] as const;
  let scaled = bytes * 100n;
  let unitIdx = 0;
  while (scaled >= 102400n && unitIdx < units.length - 1) {
    scaled = scaled / 1024n;
    unitIdx += 1;
  }
  if (unitIdx === 0) {
    return `${bytes.toString()} B`;
  }
  const whole = scaled / 100n;
  const frac = (scaled % 100n).toString().padStart(2, '0');
  return `${whole.toString()}.${frac} ${units[unitIdx]}`;
}

export function formatRateHz(rate: number | null | undefined): string {
  if (rate === null || rate === undefined || !Number.isFinite(rate)) {
    return '—';
  }
  if (rate === 0) {
    return '0 Hz';
  }
  if (rate < 10) {
    return `${rate.toFixed(2)} Hz`;
  }
  return `${rate.toFixed(1)} Hz`;
}

export function formatRatePerSec(rate: number | null | undefined, unit: string): string {
  if (rate === null || rate === undefined || !Number.isFinite(rate)) {
    return '—';
  }
  return `${rate.toFixed(1)} ${unit}`;
}
