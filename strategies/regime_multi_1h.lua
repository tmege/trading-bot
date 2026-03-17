--[[
  Regime Multi 1h — Stratégie multi-régime avec levier dynamique

  Combine détection de régime (bull/bear/neutral) avec 9 sub-stratégies
  (3 par régime). Levier dynamique x3→x5 selon la confiance.
  Position sizing ATR-based (2% risk/trade). Drawdown guard progressif.

  Architecture :
  - Régime = macro (SMA200 + EMA trend + ADX + DI)
  - Signaux = micro (conditions d'entrée per-régime)
  - Sizing = ATR-based (risque fixe 2% du capital)
  - Protection = drawdown guard progressif (pause si DD > 15%)

  Basé sur l'analyse probabiliste per-régime de 8+ ans de données.
  Signaux validés : EV > 0.8%, WR > 55%, occurrences > 30.
  Les signaux exacts proviennent de tools/regime_analyzer.py.
]]

-- ── Configuration ──────────────────────────────────────────────────────────

local config = {
    coin          = COIN or "ETH",

    -- Position sizing
    risk_per_trade = 0.02,           -- 2% du capital risqué par trade
    max_size      = 400.0,           -- cap absolu en $
    entry_size    = 80.0,            -- fallback si get_account_value échoue

    -- Leverage dynamique (ajusté par régime et confiance)
    base_leverage  = 3,
    max_leverage   = 5,

    -- Timing
    check_sec      = 60,
    cooldown_long  = 3600,           -- 1h entre longs
    cooldown_short = 7200,           -- 2h entre shorts
    max_hold_sec   = 86400,          -- 24h max hold

    -- Guards
    min_atr_pct    = 0.001,          -- ATR minimum pour trader

    -- Régime detection
    regime_confirm = 5,              -- 5 bougies consécutives pour changer (plus stable)
    adx_trend_min  = 22,             -- ADX minimum pour bull/bear (plus strict)
    adx_neutral    = 18,             -- ADX en dessous = neutral

    enabled        = true,
}

-- ── Signal Definitions ─────────────────────────────────────────────────────
-- Chaque signal: {name, side, tp, sl, regime, check(ind, mid, h)}
-- Les signaux sont filtrés par le régime détecté

local COIN_SIGNALS = {}

-- ══════════════════════════════════════════════════════════════════════════
-- ETH SIGNALS
-- ══════════════════════════════════════════════════════════════════════════
COIN_SIGNALS["ETH"] = {
    -- ── BULL REGIME ──────────────────────────────────────────────────────
    -- B1: Momentum haussier en trend confirmé
    -- ADX>25 + MACD accélérant + RSI 50-70 (pas encore overbought)
    { name = "B1_momentum", side = "long", tp = 3.0, sl = 3.0, regime = "bull",
      check = function(ind, mid, h)
          return ind.adx > 25
             and ind.macd_histogram > 0
             and h.prev_macd ~= nil and ind.macd_histogram > h.prev_macd
             and ind.rsi > 50 and ind.rsi < 70
      end },

    -- B2: Pullback dans un uptrend (achat de dip)
    -- EMA uptrend + prix < BB_middle + RSI rebond (35-50)
    { name = "B2_pullback", side = "long", tp = 2.5, sl = 3.0, regime = "bull",
      check = function(ind, mid, h)
          return ind.ema_12 > ind.ema_26
             and mid < ind.bb_middle
             and ind.rsi > 35 and ind.rsi < 50
             and ind.plus_di > ind.minus_di
      end },

    -- B3: Breakout Donchian en bull (continuation de trend)
    -- Prix fait un nouveau high 20 bars + volume confirme
    { name = "B3_breakout", side = "long", tp = 4.0, sl = 3.0, regime = "bull",
      check = function(ind, mid, h)
          return mid >= ind.dc_upper * 0.998
             and ind.adx > 25
             and ind.obv > ind.obv_sma
      end },

    -- ── BEAR REGIME ──────────────────────────────────────────────────────
    -- D1: Short momentum baissier + calme + ADX confirmé
    -- EMA downtrend + MACD < 0 + low vol + ADX > 25
    { name = "D1_bear_calm", side = "short", tp = 4.0, sl = 3.0, regime = "bear",
      check = function(ind, mid, h)
          return ind.ema_12 < ind.ema_26
             and ind.macd_histogram < 0
             and (ind.atr / mid) < 0.005
             and ind.adx > 25
      end },

    -- D2: Rally-short (contre-tendance dans un bear)
    -- Downtrend macro + prix rebondit au-dessus EMA26 + RSI > 60 + ADX confirmé
    { name = "D2_rally_short", side = "short", tp = 3.0, sl = 4.0, regime = "bear",
      check = function(ind, mid, h)
          return mid > ind.ema_26
             and ind.ema_12 < ind.ema_26
             and ind.rsi > 60
             and ind.minus_di > ind.plus_di
             and ind.adx > 25
      end },

    -- D3: Breakdown avec ADX rising
    -- BB lower + ADX fort + MACD négatif
    { name = "D3_breakdown", side = "short", tp = 5.0, sl = 4.0, regime = "bear",
      check = function(ind, mid, h)
          return mid < ind.bb_lower * 1.005
             and ind.adx > 25
             and ind.macd_histogram < 0
             and (ind.atr / mid) > 0.003
      end },

    -- ── NEUTRAL REGIME ───────────────────────────────────────────────────
    -- N1: Mean reversion long — RSI oversold en range
    { name = "N1_mr_long", side = "long", tp = 2.0, sl = 3.0, regime = "neutral",
      check = function(ind, mid, h)
          return ind.adx < 22
             and ind.rsi < 30
             and mid < ind.bb_lower * 1.005
      end },

    -- N1s: Mean reversion short — RSI overbought en range
    { name = "N1_mr_short", side = "short", tp = 2.0, sl = 3.0, regime = "neutral",
      check = function(ind, mid, h)
          return ind.adx < 22
             and ind.rsi > 70
             and mid > ind.bb_upper * 0.995
      end },

    -- N2: Squeeze breakout (BB squeeze + Keltner squeeze)
    { name = "N2_squeeze_long", side = "long", tp = 3.0, sl = 2.5, regime = "neutral",
      check = function(ind, mid, h)
          return ind.kc_squeeze
             and ind.macd_histogram > 0
             and h.prev_macd ~= nil and h.prev_macd <= 0
      end },

    { name = "N2_squeeze_short", side = "short", tp = 3.0, sl = 2.5, regime = "neutral",
      check = function(ind, mid, h)
          return ind.kc_squeeze
             and ind.macd_histogram < 0
             and h.prev_macd ~= nil and h.prev_macd >= 0
      end },
}

-- ══════════════════════════════════════════════════════════════════════════
-- SOL SIGNALS
-- ══════════════════════════════════════════════════════════════════════════
COIN_SIGNALS["SOL"] = {
    -- ── BULL REGIME ──────────────────────────────────────────────────────
    -- B1: Momentum haussier avec DI confirmé
    { name = "B1_momentum", side = "long", tp = 4.0, sl = 4.0, regime = "bull",
      check = function(ind, mid, h)
          return ind.adx > 25
             and ind.macd_histogram > 0
             and ind.plus_di > ind.minus_di
             and ind.rsi > 50 and ind.rsi < 70
      end },

    -- B2: Pullback volatil avec DI haussier (top edge SOL long)
    -- RSI < 45 + high vol + DI bull
    { name = "B2_vol_pullback", side = "long", tp = 4.0, sl = 5.0, regime = "bull",
      check = function(ind, mid, h)
          return ind.rsi < 45
             and (ind.atr / mid) > 0.012
             and ind.plus_di > ind.minus_di
      end },

    -- B3: Dip sous BB lower en trend haussier
    { name = "B3_bb_dip", side = "long", tp = 2.0, sl = 4.0, regime = "bull",
      check = function(ind, mid, h)
          return mid < ind.bb_lower * 1.005
             and ind.adx > 25
             and ind.plus_di > ind.minus_di
      end },

    -- ── BEAR REGIME ──────────────────────────────────────────────────────
    -- D1: Full bear calm — seulement si bear très confirmé (ADX > 30)
    -- EMA downtrend + MACD < 0 + low vol + ADX forte (évite faux bears)
    { name = "D1_full_bear", side = "short", tp = 5.0, sl = 3.0, regime = "bear",
      check = function(ind, mid, h)
          return ind.ema_12 < ind.ema_26
             and ind.macd_histogram < 0
             and (ind.atr / mid) < 0.005
             and ind.adx > 30
             and ind.minus_di > ind.plus_di * 1.5
      end },

    -- D2: Prix sous BB mid + faible vol + OBV déclinant + bear très confirmé
    { name = "D2_bb_obv", side = "short", tp = 5.0, sl = 3.0, regime = "bear",
      check = function(ind, mid, h)
          return mid < ind.bb_middle
             and (ind.atr / mid) < 0.005
             and ind.obv < ind.obv_sma
             and ind.adx > 30
             and ind.minus_di > ind.plus_di
      end },

    -- D3: Rally short en bear (très strict)
    { name = "D3_rally_short", side = "short", tp = 4.0, sl = 4.0, regime = "bear",
      check = function(ind, mid, h)
          return mid > ind.ema_26
             and ind.ema_12 < ind.ema_26
             and ind.rsi > 60
             and ind.minus_di > ind.plus_di
             and ind.adx > 30
      end },

    -- ── NEUTRAL REGIME ───────────────────────────────────────────────────
    -- N1: RSI/StochRSI divergent en calme (signal SOL historiquement fort)
    { name = "N1_rsi_stoch", side = "long", tp = 5.0, sl = 4.0, regime = "neutral",
      check = function(ind, mid, h)
          return ind.rsi > 60
             and ind.stoch_rsi_k < 20
             and (ind.atr / mid) < 0.003
      end },

    -- N1s: Mean reversion short
    { name = "N1_mr_short", side = "short", tp = 3.0, sl = 3.0, regime = "neutral",
      check = function(ind, mid, h)
          return ind.adx < 22
             and ind.rsi > 70
             and mid > ind.bb_upper * 0.995
      end },

    -- N2: Squeeze breakout
    { name = "N2_squeeze_long", side = "long", tp = 4.0, sl = 3.0, regime = "neutral",
      check = function(ind, mid, h)
          return ind.kc_squeeze
             and ind.macd_histogram > 0
             and h.prev_macd ~= nil and h.prev_macd <= 0
      end },

    { name = "N2_squeeze_short", side = "short", tp = 4.0, sl = 3.0, regime = "neutral",
      check = function(ind, mid, h)
          return ind.kc_squeeze
             and ind.macd_histogram < 0
             and h.prev_macd ~= nil and h.prev_macd >= 0
      end },
}

-- ── Instance Setup ─────────────────────────────────────────────────────────

local signals = COIN_SIGNALS[config.coin:upper()] or COIN_SIGNALS["ETH"]
local instance_name = "regime_multi_1h_" .. config.coin:lower()

-- ── State ──────────────────────────────────────────────────────────────────

local last_check       = 0
local last_trade       = 0
local in_position      = false
local position_side    = nil
local entry_price      = 0
local entry_time       = 0
local sl_oid           = nil
local tp_oid           = nil
local entry_oid        = nil
local entry_placed_at  = 0
local ENTRY_TIMEOUT    = 90
local trade_count      = 0
local win_count        = 0

-- Per-signal TP/SL
local active_tp        = 0
local active_sl        = 0
local active_signal    = ""
local active_leverage  = 3

-- Cooldown per-signal
local signal_last_trigger = {}

-- MACD history (inter-bougie)
local last_hour        = 0
local last_macd_val    = nil
local prev_macd        = nil
local prev2_macd       = nil

-- Régime state
local current_regime   = "neutral"
local regime_counter   = 0
local pending_regime   = "neutral"

-- Drawdown guard
local peak_equity      = 0
local dd_multiplier    = 1.0

-- ── Régime Detection ───────────────────────────────────────────────────────

local function detect_regime(ind, mid)
    local above_sma200 = mid > ind.sma_200
    local ema_bull = ind.ema_12 > ind.ema_26
    local trending = ind.adx > config.adx_trend_min
    local di_bull = ind.plus_di > ind.minus_di

    local raw
    if trending and above_sma200 and ema_bull and di_bull then
        raw = "bull"
    elseif trending and not above_sma200 and not ema_bull and not di_bull then
        raw = "bear"
    elseif ind.adx < config.adx_neutral then
        raw = "neutral"
    else
        raw = "neutral"
    end

    -- Hystérésis
    if raw == current_regime then
        regime_counter = 0
        pending_regime = current_regime
        return current_regime
    end

    if raw == pending_regime then
        regime_counter = regime_counter + 1
        if regime_counter >= config.regime_confirm then
            local old = current_regime
            current_regime = raw
            regime_counter = 0
            bot.log("info", string.format("%s: REGIME %s → %s (ADX=%.0f, DI+/DI-=%.0f/%.0f)",
                instance_name, old, raw, ind.adx, ind.plus_di, ind.minus_di))
            return current_regime
        end
    else
        pending_regime = raw
        regime_counter = 1
    end

    return current_regime
end

-- ── Dynamic Leverage ───────────────────────────────────────────────────────

local function get_leverage(regime, ind)
    local lev = config.base_leverage  -- 3x par défaut

    if regime == "neutral" then
        lev = 3
    elseif regime == "bull" or regime == "bear" then
        if regime_counter == 0 then  -- régime confirmé depuis un moment
            lev = 4
        end
        if ind.adx > 30 then
            lev = 5
        end
    end

    -- Réduire si drawdown
    lev = math.floor(lev * dd_multiplier)
    return math.max(1, math.min(lev, config.max_leverage))
end

-- ── ATR-based Position Sizing ──────────────────────────────────────────────

local function calc_size(mid, atr, leverage, sl_pct)
    local acct = bot.get_account_value()
    if not acct or acct <= 0 then return config.entry_size end

    -- Risk = 2% du capital
    local risk_usd = acct * config.risk_per_trade

    -- SL distance en $
    local sl_dist = mid * sl_pct / 100

    -- Taille basée sur le risk
    local size_usd
    if sl_dist > 0 then
        size_usd = risk_usd / (sl_dist / mid)
    else
        size_usd = acct * 0.20 * leverage
    end

    -- Caps
    size_usd = math.min(size_usd, acct * leverage * 0.5)  -- max 50% de la capacité
    size_usd = math.min(size_usd, config.max_size)
    size_usd = size_usd * dd_multiplier  -- réduire si en drawdown

    return math.max(10, size_usd)  -- minimum $10
end

-- ── Drawdown Guard ─────────────────────────────────────────────────────────

local function update_dd_guard()
    local acct = bot.get_account_value()
    if not acct or acct <= 0 then return end

    if acct > peak_equity then
        peak_equity = acct
    end

    if peak_equity <= 0 then
        dd_multiplier = 1.0
        return
    end

    local dd_pct = (peak_equity - acct) / peak_equity * 100

    if dd_pct < 5 then
        dd_multiplier = 1.0
    elseif dd_pct < 10 then
        dd_multiplier = 0.6
    elseif dd_pct < 15 then
        dd_multiplier = 0.3
    else
        dd_multiplier = 0.0  -- pause totale
        if dd_pct > 15 then
            bot.log("warn", string.format("%s: DD GUARD — %.1f%% drawdown, trading paused",
                instance_name, dd_pct))
        end
    end
end

-- ── Helpers ────────────────────────────────────────────────────────────────

local function place_entry(side, mid, leverage)
    if in_position then return nil end

    if entry_oid then
        bot.cancel(config.coin, entry_oid)
        entry_oid = nil
    end

    local price = side == "long"
        and mid * 0.9998
        or  mid * 1.0002
    local trade_usd = calc_size(mid, 0, leverage, active_sl)
    local size = trade_usd / mid
    local order_side = side == "long" and "buy" or "sell"
    local oid = bot.place_limit(config.coin, order_side, price, size, { tif = "alo" })
    if oid then
        entry_oid = oid
        entry_placed_at = bot.time()
        active_leverage = leverage
        bot.log("info", string.format("%s: ENTRY %s @ $%.2f ($%.0f, x%d) [%s/%s]",
            instance_name, string.upper(side), price, trade_usd, leverage,
            current_regime, active_signal))
        return oid
    end
    return nil
end

local function place_sl_tp(fill_price, side, pos_size, tp_pct, sl_pct)
    local sl_price, tp_price
    if side == "long" then
        sl_price = fill_price * (1 - sl_pct / 100)
        tp_price = fill_price * (1 + tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "sell", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "sell", tp_price, pos_size, tp_price, "tp")
    else
        sl_price = fill_price * (1 + sl_pct / 100)
        tp_price = fill_price * (1 - tp_pct / 100)
        sl_oid = bot.place_trigger(config.coin, "buy", sl_price, pos_size, sl_price, "sl")
        tp_oid = bot.place_trigger(config.coin, "buy", tp_price, pos_size, tp_price, "tp")
    end

    bot.log("info", string.format("%s: SL=$%.2f (-%.1f%%), TP=$%.2f (+%.1f%%) [%s]",
        instance_name, sl_price, sl_pct, tp_price, tp_pct, active_signal))
end

local function close_position(reason)
    if not in_position then return end
    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil

    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        bot.save_state("has_position", "false")
        return
    end

    local mid = bot.get_mid_price(config.coin)
    if not mid then return end

    local side = pos.size > 0 and "sell" or "buy"
    local size = math.abs(pos.size)
    local oid = bot.place_limit(config.coin, side, mid, size, { tif = "ioc", reduce_only = true })
    if not oid then
        local px = side == "sell" and mid * 0.99 or mid * 1.01
        bot.place_limit(config.coin, side, px, size, { tif = "ioc", reduce_only = true })
    end
    bot.log("info", string.format("%s: CLOSE — %s", instance_name, reason))

    in_position = false
    position_side = nil
    entry_price = 0
    bot.save_state("has_position", "false")
end

local function indicators_valid(ind)
    return ind.rsi and ind.atr and ind.macd_histogram
       and ind.adx and ind.ema_12 and ind.ema_26
       and ind.sma_20 and ind.sma_50 and ind.sma_200
       and ind.stoch_rsi_k and ind.plus_di and ind.minus_di
       and ind.bb_lower and ind.bb_middle and ind.bb_upper
       and ind.obv and ind.obv_sma
       and ind.dc_upper and ind.kc_squeeze ~= nil
end

-- ── Callbacks ──────────────────────────────────────────────────────────────

function on_init()
    if not COIN_SIGNALS[config.coin:upper()] then
        bot.log("warn", string.format("%s: coin %s non supporté (ETH/SOL)",
            instance_name, config.coin))
        config.enabled = false
    end

    bot.log("info", string.format(
        "%s: Regime Multi 1h [%d signaux, risk=%.0f%%, max=$%.0f, lev=%d-%d]",
        instance_name, #signals, config.risk_per_trade * 100,
        config.max_size, config.base_leverage, config.max_leverage))

    -- Restore state
    local saved = bot.load_state("enabled")
    if saved == "false" then config.enabled = false end
    local st = bot.load_state("trade_count")
    if st then trade_count = tonumber(st) or 0 end
    local sw = bot.load_state("win_count")
    if sw then win_count = tonumber(sw) or 0 end

    local stp = bot.load_state("active_tp")
    if stp then active_tp = tonumber(stp) or 0 end
    local ssl = bot.load_state("active_sl")
    if ssl then active_sl = tonumber(ssl) or 0 end
    active_signal = bot.load_state("active_signal") or ""

    local spe = bot.load_state("peak_equity")
    if spe then peak_equity = tonumber(spe) or 0 end
    local sreg = bot.load_state("current_regime")
    if sreg then current_regime = sreg end

    local had = bot.load_state("has_position")
    if had == "true" then
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            in_position = true
            entry_price = pos.entry_px
            position_side = pos.size > 0 and "long" or "short"
            entry_time = bot.time()
            bot.cancel_all_exchange(config.coin)
            if active_tp == 0 then active_tp = 3.0 end
            if active_sl == 0 then active_sl = 3.0 end
            place_sl_tp(entry_price, position_side, math.abs(pos.size), active_tp, active_sl)
            bot.log("info", string.format("%s: restored %s @ $%.2f [%s TP=%.1f%% SL=%.1f%%]",
                instance_name, position_side, entry_price, active_signal, active_tp, active_sl))
        else
            bot.save_state("has_position", "false")
        end
    end
    last_check = bot.time()
end

function on_tick(coin, mid_price)
    if coin ~= config.coin then return end
    if not config.enabled then return end

    local now = bot.time()
    if now - last_check < config.check_sec then return end
    last_check = now

    -- Update drawdown guard
    update_dd_guard()

    -- Pause if DD too high
    if dd_multiplier <= 0 and not in_position then return end

    -- Max hold
    if in_position and now - entry_time > config.max_hold_sec then
        close_position(string.format("max hold %ds", config.max_hold_sec))
        last_trade = now
        return
    end

    -- In position: monitor
    if in_position then
        local pos = bot.get_position(config.coin)
        if not pos or pos.size == 0 then
            in_position = false
            position_side = nil
            entry_price = 0
            bot.cancel_all_exchange(config.coin)
            sl_oid = nil
            tp_oid = nil
        end
        return
    end

    -- Pending entry guard
    if entry_oid then
        if now - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
        return
    end

    -- Guard: sync with exchange position
    local guard_pos = bot.get_position(config.coin)
    if guard_pos and guard_pos.size ~= 0 then
        in_position = true
        position_side = guard_pos.size > 0 and "long" or "short"
        entry_price = guard_pos.entry_px
        close_position("orphaned position")
        last_trade = now
        return
    end

    -- Get 1h indicators (200+ candles for SMA200)
    local ind = bot.get_indicators(config.coin, "1h", 220, mid_price)
    if not ind then return end
    if not indicators_valid(ind) then return end

    -- Min volatility guard
    local atr_pct = ind.atr / mid_price
    if atr_pct < config.min_atr_pct then return end

    -- ── Regime detection ──
    detect_regime(ind, mid_price)

    -- ── MACD history tracking ──
    local now_hour = math.floor(now / 3600)
    if now_hour ~= last_hour then
        if last_hour > 0 and last_macd_val ~= nil then
            prev2_macd = prev_macd
            prev_macd = last_macd_val
        end
        last_hour = now_hour
    end
    last_macd_val = ind.macd_histogram

    -- ── Scan signals (filter by current regime, first match wins) ──
    local hist = { prev_macd = prev_macd, prev2_macd = prev2_macd }

    for _, sig in ipairs(signals) do
        -- Skip signals not matching current regime
        if sig.regime ~= current_regime then
            goto continue_signal
        end

        -- Per-signal cooldown
        local sig_cd = sig.side == "short" and config.cooldown_short or config.cooldown_long
        local sig_last = signal_last_trigger[sig.name] or 0
        if now - sig_last < sig_cd then
            goto continue_signal
        end

        -- Global cooldown
        if now - last_trade < sig_cd then
            goto continue_signal
        end

        local ok, matched = pcall(sig.check, ind, mid_price, hist)
        if ok and matched then
            -- Dynamic leverage
            local leverage = get_leverage(current_regime, ind)

            bot.log("info", string.format(
                "%s: SIGNAL %s (%s) [%s] — RSI=%.0f ADX=%.0f ATR%%=%.3f x%d DD_mult=%.1f",
                instance_name, sig.name, string.upper(sig.side), current_regime,
                ind.rsi, ind.adx, atr_pct * 100, leverage, dd_multiplier))

            -- Store signal params
            active_tp = sig.tp
            active_sl = sig.sl
            active_signal = sig.name
            bot.save_state("active_tp", tostring(sig.tp))
            bot.save_state("active_sl", tostring(sig.sl))
            bot.save_state("active_signal", sig.name)

            signal_last_trigger[sig.name] = now
            entry_time = now
            local oid = place_entry(sig.side, mid_price, leverage)
            if oid then last_trade = now end
            return
        end

        ::continue_signal::
    end
end

function on_fill(fill)
    if fill.coin ~= config.coin then return end

    bot.log("info", string.format("%s: FILL %s %.5f @ $%.2f pnl=%.4f [%s/%s]",
        instance_name, fill.side, fill.size, fill.price, fill.closed_pnl,
        current_regime, active_signal))

    -- Entry fill
    if fill.closed_pnl == 0 then
        if not in_position then
            in_position = true
            entry_price = fill.price
            position_side = fill.side == "buy" and "long" or "short"
            entry_time = bot.time()
            bot.save_state("has_position", "true")
        end
        entry_oid = nil
        entry_placed_at = 0
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
        local pos = bot.get_position(config.coin)
        if pos and pos.size ~= 0 then
            place_sl_tp(entry_price, position_side, math.abs(pos.size), active_tp, active_sl)
        end
        return
    end

    -- Exit fill
    last_trade = bot.time()
    trade_count = trade_count + 1
    if fill.closed_pnl > 0 then
        win_count = win_count + 1
    end
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))

    bot.log("info", string.format(
        "%s: EXIT pnl=%.4f [%s/%s TP=%.1f%% SL=%.1f%%] (WR: %d/%d = %.0f%%)",
        instance_name, fill.closed_pnl, current_regime, active_signal,
        active_tp, active_sl,
        win_count, trade_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0))

    bot.cancel_all_exchange(config.coin)
    sl_oid = nil
    tp_oid = nil
    in_position = false
    position_side = nil
    entry_price = 0
    bot.save_state("has_position", "false")
end

function on_timer()
    if entry_oid and not in_position then
        if bot.time() - entry_placed_at > ENTRY_TIMEOUT then
            bot.cancel(config.coin, entry_oid)
            entry_oid = nil
        end
    end
    if not in_position then return end
    local pos = bot.get_position(config.coin)
    if not pos or pos.size == 0 then
        in_position = false
        position_side = nil
        entry_price = 0
        bot.cancel_all_exchange(config.coin)
        sl_oid = nil
        tp_oid = nil
    end
end

function on_shutdown()
    bot.log("info", string.format(
        "%s: shutdown — %d trades, %d wins (%.0f%%), regime=%s, DD_mult=%.1f",
        instance_name, trade_count, win_count,
        trade_count > 0 and (win_count / trade_count * 100) or 0,
        current_regime, dd_multiplier))
    bot.save_state("enabled", tostring(config.enabled))
    bot.save_state("trade_count", tostring(trade_count))
    bot.save_state("win_count", tostring(win_count))
    bot.save_state("has_position", tostring(in_position))
    bot.save_state("active_tp", tostring(active_tp))
    bot.save_state("active_sl", tostring(active_sl))
    bot.save_state("active_signal", active_signal or "")
    bot.save_state("peak_equity", tostring(peak_equity))
    bot.save_state("current_regime", current_regime)
end
