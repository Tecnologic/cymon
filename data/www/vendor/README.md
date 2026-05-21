# Vendor JS Libraries

These files are NOT committed to git (see .gitignore).
Download them before flashing the filesystem:

```bash
curl -Lo chart.umd.min.js \
  https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js

curl -Lo msgpack.min.js \
  https://cdn.jsdelivr.net/npm/@msgpack/msgpack@3.0.0/dist.es5+umd/msgpack.min.js

curl -Lo alpine.min.js \
  https://cdn.jsdelivr.net/npm/alpinejs@3.14.1/dist/cdn.min.js
```

The HTML falls back to CDN URLs automatically when the local files are absent
(useful during browser-based development when connected to lab WiFi).
