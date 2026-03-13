import { useState, useEffect } from 'react';

export default function useAiDigest() {
  const [digest, setDigest] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);

  useEffect(() => {
    let mounted = true;
    (async () => {
      try {
        const res = await window.api.aiDigest.get();
        if (!mounted) return;
        if (res.ok) {
          setDigest(res.data);
        } else {
          setError(res.error);
        }
      } catch (err) {
        if (mounted) setError(err.message);
      } finally {
        if (mounted) setLoading(false);
      }
    })();
    return () => { mounted = false; };
  }, []);

  return { digest, loading, error };
}
