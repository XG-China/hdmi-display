# HDMI Display 宣传素材

## 文件

- `PROMO.md`：宣传文案（B站/抖音、什么值得买/飞牛论坛、小红书/朋友圈）
- `poster-1.png`：1600×900 横版主视觉
- `poster-2.png`：1080×1080 方图（轻办公多席位）
- `poster-3.png`：1600×900 横版（按输出接口扩展）
- `poster-4.png`：1080×1920 竖版（小红书/朋友圈）
- `poster-*.html`、`posters.css`：宣传图源文件
- `render.sh`：用 Chromium headless 重新生成 PNG

## 重新生成图片

在装有 Chromium 的 Linux / NAS 上：

```bash
bash marketing/render.sh
```

Windows 可以拷贝 `marketing/` 到 NAS，或用 Docker/Chromium 打开 HTML 后截图。

## 修改文案

直接编辑对应 `poster-*.html` 里的中文文字，然后重新运行 `render.sh`。
颜色、字体、尺寸在 `posters.css` 里调整。
