import type { Video, Visibility } from './types';

export interface UserProfile {
  username: string | null;
  quota_bytes?: number;
  used_bytes?: number;
  remaining_bytes?: number;
  isAdmin?: boolean;
}

export interface AdminUser {
  username: string;
  quota_bytes: number;
  used_bytes: number;
  created_at: number;
  isAdmin: boolean;
}

export interface AdminVideo {
  id: string;
  title: string;
  owner: string;
  visibility: Visibility;
  dash: string;
  sizeBytes: number;
  duration: number;
  uploaded: number;
}

export interface AuthenticatedUserProfile extends UserProfile {
  username: string;
}

export interface PaginatedVideos {
  items: Video[];
  page: number;
  limit: number;
  totalPages: number;
  totalItems: number;
}

export type SortField = 'uploaded' | 'title' | 'author' | 'owner';
export type SortOrder = 'asc' | 'desc';

export interface ListVideosParams {
  scope?: 'all' | 'public' | 'mine';
  sortBy?: SortField;
  order?: SortOrder;
  topic?: string;
  tag?: string;
  page?: number;
  limit?: number;
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(path, init);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error((data as { error?: string }).error ?? res.statusText);
  return data as T;
}

const postForm = (fields: Record<string, string>): RequestInit => ({
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: new URLSearchParams(fields),
});

export const api = {
  me: () => request<UserProfile>('/api/me'),
  login: (username: string, password: string) =>
    request<AuthenticatedUserProfile>('/api/login', postForm({ username, password })),
  register: (username: string, password: string) =>
    request<AuthenticatedUserProfile>('/api/register', postForm({ username, password })),
  logout: () => request<object>('/api/logout', { method: 'POST' }),

  admin: {
    users: () => request<{ items: AdminUser[] }>('/api/admin/users').then((r) => r.items),
    createUser: (username: string, password: string) =>
      request<UserProfile>('/api/admin/users', postForm({ username, password })),
    deleteUser: (username: string) =>
      request<{ videosDeleted: number }>('/api/admin/users/' + encodeURIComponent(username), { method: 'DELETE' }),
    videos: () => request<{ items: AdminVideo[] }>('/api/admin/videos').then((r) => r.items),
    deleteVideo: (id: string) =>
      request<object>('/api/admin/videos/' + encodeURIComponent(id), { method: 'DELETE' }),
  },

  list: async (params: ListVideosParams = {}): Promise<PaginatedVideos> => {
    const query = new URLSearchParams();
    const normalizedScope = params.scope === 'mine' ? 'mine' : 'all';
    query.set('scope', normalizedScope);
    if (params.sortBy) query.set('sortBy', params.sortBy);
    if (params.order) query.set('order', params.order);
    if (params.topic?.trim()) query.set('topic', params.topic.trim());
    if (params.tag?.trim()) query.set('tag', params.tag.trim());
    query.set('page', String(params.page ?? 1));
    query.set('limit', String(params.limit ?? 12));

    const res = await request<PaginatedVideos | Video[]>('/api/videos?' + query.toString());
    if (Array.isArray(res)) {
      return {
        items: res,
        page: 1,
        limit: res.length || 12,
        totalPages: 1,
        totalItems: res.length,
      };
    }
    return {
      items: Array.isArray(res.items) ? res.items : [],
      page: res.page ?? 1,
      limit: res.limit ?? 12,
      totalPages: Math.max(1, res.totalPages ?? 1),
      totalItems: res.totalItems ?? (Array.isArray(res.items) ? res.items.length : 0),
    };
  },

  get: (id: string) => request<Video>('/api/videos/' + encodeURIComponent(id)),

  update: (
    id: string,
    fields: { title?: string; topic?: string; tags?: string[]; visibility?: Visibility },
  ) =>
    request<Video>('/api/videos/' + encodeURIComponent(id), {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(fields),
    }),

  remove: (id: string) =>
    request<object>('/api/videos/' + encodeURIComponent(id), { method: 'DELETE' }),
};

/** Upload with progress reporting (fetch cannot report upload progress). */
export function uploadVideo(
  file: File,
  title: string,
  visibility: Visibility,
  onProgress: (percent: number) => void,
  extra?: { topic?: string; tags?: string },
): Promise<Video> {
  return new Promise((resolve, reject) => {
    const params: Record<string, string> = {
      filename: file.name,
      title,
      visibility,
    };
    if (extra?.topic?.trim()) params.topic = extra.topic.trim();
    if (extra?.tags?.trim()) params.tags = extra.tags.trim();

    const qs = new URLSearchParams(params);
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