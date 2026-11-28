import { describe, expect, it } from 'vitest';
import {
  formatDecimalCount,
  formatIecBytes,
  formatRateHz,
  isPositiveUint64Decimal,
  isValidUint64Decimal,
  parseUint64BigInt,
} from '../../src/format/decimal';
import { formatAgeSeconds, formatDurationMs, formatTimestampUtc } from '../../src/format/time';
import {
  classifyFailureKind,
  parseApiVersionHeader,
  parseRetryAfterMs,
} from '../../src/api/errors';

describe('numeric and uint64 formatting', () => {
  it('formats both bounded JSON numbers and uint64 DecimalStrings cleanly', () => {
    const beyondSafe = '9007199254740993';
    const maxU64 = '18446744073709551615';
    expect(isValidUint64Decimal(beyondSafe)).toBe(true);
    expect(isPositiveUint64Decimal(beyondSafe)).toBe(true);
    expect(parseUint64BigInt(beyondSafe)?.toString()).toBe(beyondSafe);
    expect(formatDecimalCount(beyondSafe)).toBe('9,007,199,254,740,993');
    expect(formatDecimalCount(123456)).toBe('123,456');

    expect(isValidUint64Decimal(maxU64)).toBe(true);
    expect(formatDecimalCount(maxU64)).toBe('18,446,744,073,709,551,615');
  });

  it('rejects overflow, negative, leading zeros, and non-decimal strings', () => {
    expect(isValidUint64Decimal('18446744073709551616')).toBe(false);
    expect(isValidUint64Decimal('-1')).toBe(false);
    expect(isValidUint64Decimal('01')).toBe(false);
    expect(isValidUint64Decimal('1e6')).toBe(false);
    expect(isPositiveUint64Decimal('0')).toBe(false);
    expect(isValidUint64Decimal('0')).toBe(true);
  });

  it('formats IEC bytes and Hz rates deterministically', () => {
    expect(formatIecBytes(0)).toBe('0 B');
    expect(formatIecBytes(1024)).toBe('1.00 KiB');
    expect(formatIecBytes(1572864)).toBe('1.50 MiB');
    expect(formatIecBytes('9007199254740993')).toContain('PiB');
    expect(formatIecBytes(null)).toBe('—');
    expect(formatRateHz(0)).toBe('0 Hz');
    expect(formatRateHz(2.5)).toBe('2.50 Hz');
    expect(formatRateHz(30.44)).toBe('30.4 Hz');
  });
});

describe('duration and time formatters', () => {
  it('formats integer milliseconds cleanly', () => {
    expect(formatDurationMs(null)).toBe('—');
    expect(formatDurationMs(0)).toBe('0 ms');
    expect(formatDurationMs(500)).toBe('500 ms');
    expect(formatDurationMs(1500)).toBe('1.50 s');
    expect(formatDurationMs(125_000)).toBe('2m 5s');
  });

  it('formats UTC timestamps and age', () => {
    expect(formatTimestampUtc('2026-11-27T12:34:56.789Z')).toBe('2026-11-27 12:34:56Z');
    expect(formatAgeSeconds(100_000, 95_000)).toBe('5s ago');
    expect(formatAgeSeconds(200_000, 75_000)).toBe('2m 5s ago');
  });
});

describe('error classification and header parsers', () => {
  it('parses Retry-After delta-seconds and HTTP-date', () => {
    expect(parseRetryAfterMs('7')).toBe(7000);
    expect(parseRetryAfterMs('0')).toBe(0);
    expect(parseRetryAfterMs('999999')).toBe(300_000);
    const now = Date.parse('2026-11-27T12:00:00Z');
    expect(parseRetryAfterMs('Fri, 27 Nov 2026 12:00:15 GMT', now)).toBe(15_000);
    expect(parseRetryAfterMs('invalid-header', now)).toBeNull();
  });

  it('classifies API failures and parses RoDAQ-API-Version', () => {
    expect(classifyFailureKind(401, null)).toBe('unauthenticated');
    expect(classifyFailureKind(403, null)).toBe('forbidden');
    expect(classifyFailureKind(409, null)).toBe('validation');
    expect(classifyFailureKind(504, null)).toBe('timeout');
    expect(
      classifyFailureKind(400, {
        code: 'unsupported_api_version',
        message: 'major mismatch',
        retryability: 'never',
        failure_domain: 'request',
        data_impact: 'none',
        details: [],
      }),
    ).toBe('api_incompatible');
    expect(parseApiVersionHeader('1.2')).toEqual({ major: 1, minor: 2 });
    expect(parseApiVersionHeader('v1.0')).toBeNull();
  });
});
