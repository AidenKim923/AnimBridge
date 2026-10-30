import en from './locales/en.js';
import es from './locales/es.js';
import ja from './locales/ja.js';
import ko from './locales/ko.js';
import zhCN from './locales/zh-CN.js';
import zhTW from './locales/zh-TW.js';
import pl from './locales/pl.js';
import hi from './locales/hi.js';
import ru from './locales/ru.js';
import ar from './locales/ar.js';
import ptBR from './locales/pt-BR.js';
import ptPT from './locales/pt-PT.js';
import fr from './locales/fr.js';
import th from './locales/th.js';
import tr from './locales/tr.js';
import nativeEn from './locales/native-en.js';
import nativeKo from './locales/native-ko.js';

export const SUPPORTED_LOCALES = Object.freeze([
  { code: 'en', name: 'English', dir: 'ltr' },
  { code: 'es', name: 'Español', dir: 'ltr' },
  { code: 'ja', name: '日本語', dir: 'ltr' },
  { code: 'ko', name: '한국어', dir: 'ltr' },
  { code: 'zh-CN', name: '简体中文', dir: 'ltr' },
  { code: 'zh-TW', name: '繁體中文', dir: 'ltr' },
  { code: 'pl', name: 'Polski', dir: 'ltr' },
  { code: 'hi', name: 'हिन्दी', dir: 'ltr' },
  { code: 'ru', name: 'Русский', dir: 'ltr' },
  { code: 'ar', name: 'العربية', dir: 'rtl' },
  { code: 'pt-BR', name: 'Português (Brasil)', dir: 'ltr' },
  { code: 'pt-PT', name: 'Português (Portugal)', dir: 'ltr' },
  { code: 'fr', name: 'Français', dir: 'ltr' },
  { code: 'th', name: 'ไทย', dir: 'ltr' },
  { code: 'tr', name: 'Türkçe', dir: 'ltr' },
]);
const catalogs = {
  en: { ...en, ...nativeEn },
  es,
  ja,
  ko: { ...ko, ...nativeKo },
  'zh-CN': zhCN,
  'zh-TW': zhTW,
  pl,
  hi,
  ru,
  ar,
  'pt-BR': ptBR,
  'pt-PT': ptPT,
  fr,
  th,
  tr,
};
let locale = 'en';

export function normalizeLocale(value) {
  return typeof value === 'string'
    ? SUPPORTED_LOCALES.find((item) => item.code.toLowerCase() === value.toLowerCase())?.code ||
        'en'
    : 'en';
}
export function setLocale(value) {
  locale = normalizeLocale(value);
  if (typeof document !== 'undefined') {
    document.documentElement.lang = locale;
    document.documentElement.dir = locale === 'ar' ? 'rtl' : 'ltr';
  }
  return locale;
}
export function getLocale() {
  return locale;
}
export function formatNumber(value, options = {}) {
  const number = Number(value);
  return Number.isFinite(number)
    ? new Intl.NumberFormat(locale, options).format(number)
    : String(value ?? '');
}
export function formatDate(
  value,
  options = { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' },
) {
  if (value === null || value === undefined || value === '') return '';
  const date =
    value instanceof Date
      ? value
      : new Date(typeof value === 'number' && value < 1e12 ? value * 1000 : value);
  return Number.isNaN(date.getTime()) ? '' : new Intl.DateTimeFormat(locale, options).format(date);
}
// Messages stay plain text. HTML callers escape the result at the insertion point.
export function t(key, params = {}) {
  const ownString = (catalog) =>
    Object.hasOwn(catalog, key) && typeof catalog[key] === 'string' ? catalog[key] : undefined;
  const text = ownString(catalogs[locale]) ?? ownString(catalogs.en) ?? String(key);
  return text.replace(/\{([A-Za-z][A-Za-z0-9_]*)\}/g, (placeholder, name) =>
    Object.hasOwn(params, name)
      ? typeof params[name] === 'number'
        ? formatNumber(params[name])
        : String(params[name] ?? '')
      : placeholder,
  );
}
