export type Visibility = 'public' | 'private';
export type DashStatus = 'unavailable' | 'processing' | 'ready' | 'failed';

export interface VideoMeta {
  filename: string;
  size: number;
  mime: string;
  duration: number;
  width: number;
  height: number;
  vcodec: string;
  acodec: string;
  brand: string;
  tracks: number;
  created: number;
  bitrate: number;
}

export interface Video {
  id: string;
  title: string;
  owner: string;
  visibility: Visibility;
  uploaded: number;
  isOwner: boolean;
  dash: DashStatus;
  meta: VideoMeta;
}

export type View = 'browse' | 'mine' | 'upload';
