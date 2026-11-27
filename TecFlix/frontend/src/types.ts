export type Visibility = 'public' | 'private';
export type DashStatus = 'unavailable' | 'pending' | 'processing' | 'ready' | 'failed' | 'error';

/** Forma real que devuelve la API de metadata (/api/videos). */
export interface Video {
  id: string;
  title: string;
  owner: string;
  topic: string;
  tags: string[];
  visibility: Visibility;
  file: string;
  dash: DashStatus;
  duration: number;
  sizeBytes: number;
  uploaded: number;
  canEdit: boolean;
}

export type View = 'browse' | 'mine' | 'upload' | 'admin';
