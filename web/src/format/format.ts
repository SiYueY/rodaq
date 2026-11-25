import type { DecimalString, DurationValue, StreamRuntimeData } from '../api/types'

const KIB = 1024n
const MIB = KIB * 1024n
const GIB = MIB * 1024n
const TIB = GIB * 1024n

// Format uint64 decimal-string bytes using BigInt (never Number() on raw uint64).
export function formatBytesDecimal(bytes: DecimalString | null | undefined): string {
  if (!bytes || !/^\d+$/.test(bytes)) return '—'
  const v = BigInt(bytes)
  if (v < KIB) return `${v.toString()} B`
  const scale = (unit: bigint, label: string) => {
    const whole = v / unit
    const frac = ((v % unit) * 10n) / unit
    return `${whole.toString()}.${frac.toString()} ${label}`
  }
  if (v < MIB) return scale(KIB, 'KiB')
  if (v < GIB) return scale(MIB, 'MiB')
  if (v < TIB) return scale(GIB, 'GiB')
  return scale(TIB, 'TiB')
}

export function formatDuration(dur: DurationValue | null | undefined): string {
  if (!dur) return '—'
  return `${dur.value} ${dur.unit}`
}

export function durationToMs(dur: DurationValue | null | undefined): number {
  if (!dur || !/^\d+$/.test(dur.value)) return 0
  const v = BigInt(dur.value)
  switch (dur.unit) {
    case 'ns':
      return Number(v / 1_000_000n)
    case 'us':
      return Number(v / 1_000n)
    case 'ms':
      return Number(v)
    case 's':
      return Number(v * 1_000n)
  }
}

export function formatRateHz(rps: number): string {
  if (!Number.isFinite(rps) || rps < 0) return '—'
  return `${rps.toFixed(1)} Hz`
}

// Web §2.4: 0 Hz + source not ready vs 0 Hz + source ready/silent must be explicitly distinguished.
export function formatStreamRateLabel(stream: StreamRuntimeData): string {
  const rps = stream.received.records_per_second
  if (rps === 0) {
    return stream.source_ready ? '0 Hz · source present/silent' : '0 Hz · no source'
  }
  return formatRateHz(rps)
}
