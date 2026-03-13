import React from 'react';
import { Sparkles, TrendingUp, TrendingDown, Minus, Zap, Eye } from 'lucide-react';
import useAiDigest from '../hooks/useAiDigest';

const SENTIMENT_STYLES = {
  bullish: {
    color: 'text-profit',
    bg: 'bg-profit/15',
    border: 'border-profit/30',
    icon: TrendingUp,
    label: 'Bullish',
  },
  bearish: {
    color: 'text-loss',
    bg: 'bg-loss/15',
    border: 'border-loss/30',
    icon: TrendingDown,
    label: 'Bearish',
  },
  neutre: {
    color: 'text-yellow-400',
    bg: 'bg-yellow-400/15',
    border: 'border-yellow-400/30',
    icon: Minus,
    label: 'Neutre',
  },
};

function SkeletonLine({ width = 'w-full' }) {
  return <div className={`h-3 bg-surface-bg rounded animate-pulse ${width}`} />;
}

export default function AiDigest() {
  const { digest, loading, error } = useAiDigest();

  // No API key configured
  if (!loading && error === 'no_api_key') {
    return (
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
          <Sparkles size={12} />
          AI Daily Digest
        </h3>
        <div className="text-sm text-gray-400 bg-surface-bg rounded-lg p-3">
          <p className="mb-1">Configurez <code className="text-yellow-400 text-xs">TB_ANTHROPIC_API_KEY</code> dans votre fichier <code className="text-yellow-400 text-xs">.env</code> pour activer le digest IA.</p>
          <p className="text-[10px] text-gray-600">Analyse automatique des news crypto via Claude AI.</p>
        </div>
      </div>
    );
  }

  // Loading state
  if (loading && !digest) {
    return (
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
          <Sparkles size={12} className="animate-pulse" />
          AI Daily Digest
          <span className="ml-auto text-[10px] text-gray-600">Analyse en cours...</span>
        </h3>
        <div className="space-y-2.5">
          <SkeletonLine width="w-3/4" />
          <SkeletonLine />
          <SkeletonLine width="w-5/6" />
          <SkeletonLine width="w-2/3" />
        </div>
      </div>
    );
  }

  // Error state
  if (error && !digest) {
    return (
      <div className="bg-surface-card border border-surface-border rounded-lg p-4">
        <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
          <Sparkles size={12} />
          AI Daily Digest
        </h3>
        <div className="text-sm text-loss bg-loss/10 rounded-lg p-3">
          Erreur : {error}
        </div>
      </div>
    );
  }

  if (!digest) return null;

  const sentiment = SENTIMENT_STYLES[digest.sentiment] || SENTIMENT_STYLES.neutre;
  const SentimentIcon = sentiment.icon;

  return (
    <div className="bg-surface-card border border-surface-border rounded-lg p-4">
      {/* Header */}
      <h3 className="text-xs text-gray-500 uppercase tracking-wider mb-3 flex items-center gap-2">
        <Sparkles size={12} className="text-purple-400" />
        AI Daily Digest
        {digest.article_count > 0 && (
          <span className="text-[10px] text-gray-600 normal-case">
            ({digest.article_count} articles analyses)
          </span>
        )}
      </h3>

      {/* Sentiment badge */}
      <div className={`${sentiment.bg} border ${sentiment.border} rounded-lg px-3 py-2 mb-3 flex items-center gap-2`}>
        <SentimentIcon size={14} className={sentiment.color} />
        <span className={`text-xs font-bold ${sentiment.color}`}>{sentiment.label}</span>
        {digest.sentiment_reason && (
          <span className="text-[10px] text-gray-400 ml-1">— {digest.sentiment_reason}</span>
        )}
      </div>

      {/* Key points */}
      {digest.points?.length > 0 && (
        <div className="space-y-2 mb-3">
          {digest.points.map((point, i) => (
            <div key={i} className="flex gap-2 text-sm text-gray-300">
              <span className="text-purple-400 mt-0.5 shrink-0">•</span>
              <span>{point}</span>
            </div>
          ))}
        </div>
      )}

      {/* Key events */}
      {digest.events?.length > 0 && (
        <div className="bg-surface-bg rounded-lg p-3 mb-3">
          <div className="text-[10px] text-gray-500 uppercase tracking-wider mb-1.5 flex items-center gap-1">
            <Zap size={10} />
            Evenements cles
          </div>
          <div className="space-y-1">
            {digest.events.map((event, i) => (
              <div key={i} className="text-xs text-gray-300 flex gap-1.5">
                <span className="text-yellow-400">▸</span>
                {event}
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Trends */}
      {digest.trends?.length > 0 && (
        <div className="bg-surface-bg rounded-lg p-3 mb-3">
          <div className="text-[10px] text-gray-500 uppercase tracking-wider mb-1.5 flex items-center gap-1">
            <Eye size={10} />
            Tendances
          </div>
          <div className="space-y-1">
            {digest.trends.map((trend, i) => (
              <div key={i} className="text-xs text-gray-400 flex gap-1.5">
                <span className="text-purple-400">→</span>
                {trend}
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Footer: sources + timestamp */}
      <div className="flex items-center justify-between text-[10px] text-gray-600">
        <span>
          Sources : {digest.sources?.join(', ')}
        </span>
        {digest.generated_at && (
          <span>
            {new Date(digest.generated_at).toLocaleString('fr-FR', {
              day: '2-digit',
              month: '2-digit',
              hour: '2-digit',
              minute: '2-digit',
            })}
          </span>
        )}
      </div>
    </div>
  );
}
