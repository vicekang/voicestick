<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { setLocale } from './i18n'
import productPhoto from './assets/sticks3.png'

const { locale, t } = useI18n()
const releaseUrl = 'https://github.com/78/voicestick/releases/latest'
const githubUrl = 'https://github.com/78/voicestick'

const languageLabel = computed(() => (locale.value === 'zh-CN' ? t('language.en') : t('language.zh')))
const nextLocale = computed(() => (locale.value === 'zh-CN' ? 'en-US' : 'zh-CN'))

function toggleLanguage() {
  setLocale(nextLocale.value)
}
</script>

<template>
  <header class="topbar">
    <div class="topbar-inner">
      <a class="brand" href="./" aria-label="VoiceStick">
        <span class="brand-mark" aria-hidden="true"></span>
        <span>VoiceStick</span>
      </a>
      <nav>
        <a :href="releaseUrl">{{ t('nav.download') }}</a>
        <a :href="githubUrl">{{ t('nav.github') }}</a>
        <button class="language-button" type="button" :aria-label="t('language.label')" @click="toggleLanguage">
          {{ languageLabel }}
        </button>
      </nav>
    </div>
  </header>

  <main>
    <section class="hero">
      <div class="hero-copy">
        <p class="eyebrow">{{ t('hero.eyebrow') }}</p>
        <h1>{{ t('hero.title') }}</h1>
        <p class="lead">{{ t('hero.lead') }}</p>
        <div class="actions">
          <a class="button primary" :href="releaseUrl">{{ t('hero.download') }}</a>
          <a class="button secondary" :href="githubUrl">{{ t('hero.source') }}</a>
        </div>
      </div>
      <div class="product-visual" :aria-label="t('hero.imageAlt')">
        <img class="product-photo" :src="productPhoto" :alt="t('hero.imageAlt')">
      </div>
    </section>

    <section class="features" :aria-label="t('features.label')">
      <div class="section-inner features-grid">
        <article>
          <h2>{{ t('features.ble.title') }}</h2>
          <p>{{ t('features.ble.body') }}</p>
        </article>
        <article>
          <h2>{{ t('features.model.title') }}</h2>
          <p>{{ t('features.model.body') }}</p>
        </article>
        <article>
          <h2>{{ t('features.feedback.title') }}</h2>
          <p>{{ t('features.feedback.body') }}</p>
        </article>
      </div>
    </section>

    <section class="story">
      <div class="section-inner story-grid">
        <div>
          <p class="eyebrow">{{ t('story.eyebrow') }}</p>
          <h2>{{ t('story.title') }}</h2>
        </div>
        <p>{{ t('story.body') }}</p>
      </div>
    </section>

    <section class="download-band" id="download">
      <div class="section-inner download-inner">
        <div>
          <h2>{{ t('download.title') }}</h2>
          <p>{{ t('download.body') }}</p>
        </div>
        <a class="button primary" :href="releaseUrl">{{ t('download.cta') }}</a>
      </div>
    </section>
  </main>

  <footer>
    <div class="section-inner footer-inner">
      <span>VoiceStick</span>
      <a href="./appcast.xml">{{ t('footer.appcast') }}</a>
    </div>
  </footer>
</template>
