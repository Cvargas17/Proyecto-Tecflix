import { useEffect, useRef, useState } from 'react';
import { api, uploadVideo, type UserProfile } from '../api';
import type { Visibility } from '../types';

interface Props {
  user: UserProfile | null;
  onUploaded: () => void;
}

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export default function UploadView({ user, onUploaded }: Props) {
  const fileRef = useRef<HTMLInputElement>(null);
  const [profile, setProfile] = useState<UserProfile | null>(user);
  const [selectedFileSize, setSelectedFileSize] = useState<number>(0);
  const [title, setTitle] = useState('');
  const [topic, setTopic] = useState('');
  const [tags, setTags] = useState('');
  const [visibility, setVisibility] = useState<Visibility>('private');
  const [progress, setProgress] = useState<number | null>(null);
  const [error, setError] = useState('');

  useEffect(() => {
    setProfile(user);
    api.me()
      .then((fresh) => {
        if (fresh.username) setProfile(fresh);
      })
      .catch(() => {});
  }, [user]);

  const hasQuotaInfo =
    typeof profile?.quota_bytes === 'number' && profile.quota_bytes > 0;
  const usedBytes = profile?.used_bytes ?? 0;
  const quotaBytes = profile?.quota_bytes ?? 0;
  const remainingBytes =
    typeof profile?.remaining_bytes === 'number'
      ? profile.remaining_bytes
      : Math.max(0, quotaBytes - usedBytes);

  const quotaReached = hasQuotaInfo && remainingBytes === 0;
  const exceedsQuota = hasQuotaInfo && selectedFileSize > remainingBytes;
  const usagePercent = hasQuotaInfo
    ? Math.min(100, Math.round((usedBytes / quotaBytes) * 100))
    : 0;

  const handleFileChange = () => {
    const file = fileRef.current?.files?.[0];
    setSelectedFileSize(file ? file.size : 0);
    setError('');
  };

  const submit = async () => {
    const file = fileRef.current?.files?.[0];
    setError('');
    if (!file) {
      setError('Choose a video file first.');
      return;
    }
    if (hasQuotaInfo && file.size > remainingBytes) {
      setError(
        `Storage quota exceeded. File is ${formatBytes(file.size)}, but only ${formatBytes(remainingBytes)} remaining.`,
      );
      return;
    }

    setProgress(0);
    try {
      await uploadVideo(file, title, visibility, setProgress, { topic, tags });
      onUploaded();
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Upload failed');
    } finally {
      setProgress(null);
    }
  };

  return (
    <section>
      <h2>Upload a video</h2>

      {hasQuotaInfo && (
        <div
          style={{
            marginBottom: '1rem',
            padding: '0.75rem',
            borderRadius: '6px',
            background: 'rgba(255,255,255,0.05)',
          }}
        >
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '0.35rem' }}>
            <span>Storage Quota (Fixed)</span>
            <span>
              {formatBytes(usedBytes)} used / {formatBytes(quotaBytes)} total ({formatBytes(remainingBytes)} free)
            </span>
          </div>
          <progress value={usagePercent} max={100} style={{ width: '100%' }} />
        </div>
      )}

      <div className="form">
        <label>Video file (mp4, m4v, mov, webm, ogv, mkv)</label>
        <input
          ref={fileRef}
          type="file"
          accept=".mp4,.m4v,.mov,.webm,.ogv,.ogg,.mkv,video/*"
          disabled={quotaReached || progress !== null}
          onChange={handleFileChange}
        />

        <label>Title</label>
        <input
          type="text"
          value={title}
          maxLength={120}
          placeholder="Defaults to the file name"
          onChange={(e) => setTitle(e.target.value)}
        />

        <label>Topic (optional)</label>
        <input
          type="text"
          value={topic}
          maxLength={60}
          placeholder="e.g. Algorithms, Systems, Music"
          onChange={(e) => setTopic(e.target.value)}
        />

        <label>Tags (optional, comma-separated)</label>
        <input
          type="text"
          value={tags}
          placeholder="e.g. cpp, tutorial, demo"
          onChange={(e) => setTags(e.target.value)}
        />

        <label>Visibility</label>
        <select value={visibility} onChange={(e) => setVisibility(e.target.value as Visibility)}>
          <option value="private">Private (only me)</option>
          <option value="public">Public (everyone)</option>
        </select>

        <button
          className="btn red"
          disabled={progress !== null || quotaReached || exceedsQuota}
          onClick={() => void submit()}
        >
          Upload
        </button>

        {progress !== null && <progress value={progress} max={100} />}
        <div className="err">
          {quotaReached
            ? 'You have reached your fixed storage quota and cannot upload more videos.'
            : exceedsQuota
              ? `Selected file (${formatBytes(selectedFileSize)}) exceeds remaining quota (${formatBytes(remainingBytes)}).`
              : error}
        </div>
      </div>
    </section>
  );
}