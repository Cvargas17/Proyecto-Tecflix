export function fmtSize(bytes: number): string {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (bytes >= 1024 && i < units.length - 1) { bytes /= 1024; i++; }
  return `${bytes.toFixed(i ? 1 : 0)} ${units[i]}`;
}

export function fmtDuration(seconds: number): string {
  if (!seconds) return 'Unknown';
  const s = Math.round(seconds);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  const sec = String(x).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

export const fmtDate = (epoch: number): string => (epoch ? new Date(epoch * 1000).toLocaleString() : 'Unknown');

const gcd = (a: number, b: number): number => (b ? gcd(b, a % b) : a);

export function fmtResolution(w: number, h: number): string {
  if (!w || !h) return 'Unknown';
  const g = gcd(w, h);
  return `${w} × ${h} (${w / g}:${h / g})`;
}
