import { useState, useEffect } from 'react';

export default function useBotStatus() {
  const [status, setStatus] = useState({
    running: false,
    pid: null,
    uptimeSec: 0,
  });

  useEffect(() => {
    let mounted = true;

    async function poll() {
      try {
        const s = await window.api.bot.status();
        if (mounted) setStatus(s);
      } catch (_) {
        // api not available (dev without electron)
      }
    }

    poll();
    const id = setInterval(poll, 2000);
    return () => { mounted = false; clearInterval(id); };
  }, []);

  return status;
}
