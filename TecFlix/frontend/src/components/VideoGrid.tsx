import { useEffect, useState } from 'react';
import { api, type PaginatedVideos, type SortField, type SortOrder } from '../api';
import VideoCard from './VideoCard';

interface Props {
  scope: 'all' | 'public' | 'mine';
  reloadKey: number;
  onOpen: (id: string) => void;
}

export default function VideoGrid({ scope, reloadKey, onOpen }: Props) {
  const [data, setData] = useState<PaginatedVideos | null>(null);
  const [sortBy, setSortBy] = useState<SortField>('uploaded');
  const [order, setOrder] = useState<SortOrder>('desc');
  const [topic, setTopic] = useState('');
  const [page, setPage] = useState(1);
  const [limit, setLimit] = useState(12);
  const [error, setError] = useState('');

  useEffect(() => {
    setPage(1);
  }, [scope, sortBy, order, topic, limit]);

  useEffect(() => {
    let alive = true;
    setError('');
    api
      .list({ scope, sortBy, order, topic, page, limit })
      .then((res) => alive && setData(res))
      .catch((e: Error) => alive && setError(e.message));
    return () => {
      alive = false;
    };
  }, [scope, sortBy, order, topic, page, limit, reloadKey]);

  return (
    <div>
      <div
        style={{
          display: 'flex',
          flexWrap: 'wrap',
          gap: '0.75rem',
          alignItems: 'center',
          marginBottom: '1rem',
        }}
      >
        <label style={{ display: 'flex', alignItems: 'center', gap: '0.4rem' }}>
          <span className="dim">Sort by:</span>
          <select value={sortBy} onChange={(e) => setSortBy(e.target.value as SortField)}>
            <option value="uploaded">Upload date</option>
            <option value="title">Title</option>
            <option value="author">Author</option>
          </select>
        </label>

        <label style={{ display: 'flex', alignItems: 'center', gap: '0.4rem' }}>
          <span className="dim">Order:</span>
          <select value={order} onChange={(e) => setOrder(e.target.value as SortOrder)}>
            <option value="desc">Descending</option>
            <option value="asc">Ascending</option>
          </select>
        </label>

        <input
          type="text"
          value={topic}
          placeholder="Filter by topic..."
          onChange={(e) => setTopic(e.target.value)}
          style={{ maxWidth: '200px' }}
        />

        <label style={{ display: 'flex', alignItems: 'center', gap: '0.4rem', marginLeft: 'auto' }}>
          <span className="dim">Per page:</span>
          <select value={limit} onChange={(e) => setLimit(Number(e.target.value))}>
            <option value={6}>6</option>
            <option value={12}>12</option>
            <option value={24}>24</option>
          </select>
        </label>
      </div>

      {error && <div className="empty">{error}</div>}
      {!error && !data && <div className="empty">Loading…</div>}
      {!error && data && data.items.length === 0 && (
        <div className="empty">
          {scope === 'mine' ? 'You have not uploaded anything yet.' : 'No videos found.'}
        </div>
      )}

      {!error && data && data.items.length > 0 && (
        <>
          <div className="grid">
            {data.items.map((v) => (
              <VideoCard key={v.id} video={v} onOpen={onOpen} />
            ))}
          </div>

          <div
            style={{
              display: 'flex',
              justifyContent: 'center',
              alignItems: 'center',
              gap: '1rem',
              marginTop: '1.5rem',
            }}
          >
            <button
              className="btn"
              disabled={page <= 1}
              onClick={() => setPage((p) => Math.max(1, p - 1))}
            >
              Previous
            </button>
            <span className="dim">
              Page {data.page} of {data.totalPages} ({data.totalItems} videos)
            </span>
            <button
              className="btn"
              disabled={page >= data.totalPages}
              onClick={() => setPage((p) => Math.min(data.totalPages, p + 1))}
            >
              Next
            </button>
          </div>
        </>
      )}
    </div>
  );
}