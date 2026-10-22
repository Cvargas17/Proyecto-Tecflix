import type { Video, Visibility } from './types';

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(path, init);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error((data as { error?: string }).error ?? res.statusText);
  return data as T;
}

const post = (fields: Record<string, string>): RequestInit => ({
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: new URLSearchParams(fields),
});

export const api = {
  me: () => request<{ username: string | null }>('/api/me'),
  login: (username: string, password: string) =>
    request<{ username: string }>('/api/login', post({ username, password })),
  register: (username: string, password: string) =>
    request<{ username: string }>('/api/register', post({ username, password })),
  logout: () => request<object>('/api/logout', { method: 'POST' }),
  list: (scope: 'public' | 'mine') => request<Video[]>('/api/videos' + (scope === 'mine' ? '?scope=mine' : '')),
  get: (id: string) => request<Video>('/api/videos/' + id),
  update: (id: string, fields: { title?: string; visibility?: Visibility }) =>
    request<Video>('/api/videos/' + id, post(fields as Record<string, string>)),
  remove: (id: string) => request<object>('/api/videos/' + id, { method: 'DELETE' }),
};

/** Upload with progress reporting (fetch cannot report upload progress). */
export function uploadVideo(
  file: File,
  title: string,
  visibility: Visibility,
  onProgress: (percent: number) => void,
): Promise<Video> {
  return new Promise((resolve, reject) => {
    const qs = new URLSearchParams({ filename: file.name, title, visibility });
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/upload?' + qs);
    xhr.upload.onprogress = (e) => e.lengthComputable && onProgress((e.loaded / e.total) * 100);
    xhr.onload = () => {
      let body: { error?: string } = {};
      try { body = JSON.parse(xhr.responseText); } catch { /* keep empty */ }
      if (xhr.status === 201) resolve(body as unknown as Video);
      else reject(new Error(body.error ?? 'Upload failed'));
    };
    xhr.onerror = () => reject(new Error('Network error during upload'));
    xhr.send(file);
  });
}
