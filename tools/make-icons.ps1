# Regenerates res\app.ico and res\paused.ico (multi-size, PNG entries) and a preview strip.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make-icons.ps1 [-Preview path.png]
param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\res'),
    [string]$Preview = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

public static class IconMaker {
    static readonly int[] Sizes = { 16, 20, 24, 32, 40, 48, 64, 256 };

    static Color Tone(int r, int g, int b, bool gray) {
        if (!gray) return Color.FromArgb(255, r, g, b);
        int l = (int)(0.299 * r + 0.587 * g + 0.114 * b);
        return Color.FromArgb(255, l, l, l);
    }

    static GraphicsPath Rounded(RectangleF r, float radius) {
        var p = new GraphicsPath();
        float d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    // A monitor with a "minimize" glyph, drawn on a 16 x 16 design grid.
    public static Bitmap Draw(int size, bool gray) {
        var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.Clear(Color.Transparent);
            float u = size / 16f;
            var bezel = new RectangleF(0.5f * u, 1.4f * u, 15f * u, 10.6f * u);
            using (var path = Rounded(bezel, 1.7f * u))
            using (var brush = new SolidBrush(Tone(30, 41, 59, gray))) g.FillPath(brush, path);
            var screen = new RectangleF(1.55f * u, 2.45f * u, 12.9f * u, 8.5f * u);
            using (var path = Rounded(screen, 0.9f * u))
            using (var brush = new LinearGradientBrush(screen, Tone(96, 165, 250, gray), Tone(37, 99, 235, gray), LinearGradientMode.Vertical))
                g.FillPath(brush, path);
            using (var brush = new SolidBrush(Tone(100, 116, 139, gray))) {
                g.FillRectangle(brush, 7f * u, 11.9f * u, 2f * u, 1.7f * u);
                using (var path = Rounded(new RectangleF(4.4f * u, 13.3f * u, 7.2f * u, 1.5f * u), 0.7f * u)) g.FillPath(brush, path);
            }
            using (var pen = new Pen(Color.White, 1.55f * u)) {
                pen.StartCap = LineCap.Round;
                pen.EndCap = LineCap.Round;
                pen.LineJoin = LineJoin.Round;
                g.DrawLines(pen, new[] { new PointF(5.2f * u, 4.6f * u), new PointF(8f * u, 7.3f * u), new PointF(10.8f * u, 4.6f * u) });
                g.DrawLine(pen, 5.2f * u, 9.25f * u, 10.8f * u, 9.25f * u);
            }
        }
        return bmp;
    }

    public static void WriteIco(string path, bool gray) {
        var images = new byte[Sizes.Length][];
        for (int i = 0; i < Sizes.Length; i++)
            using (var bmp = Draw(Sizes[i], gray))
            using (var ms = new MemoryStream()) {
                bmp.Save(ms, ImageFormat.Png);
                images[i] = ms.ToArray();
            }
        using (var fs = new FileStream(path, FileMode.Create))
        using (var w = new BinaryWriter(fs)) {
            w.Write((short)0);
            w.Write((short)1);
            w.Write((short)Sizes.Length);
            int offset = 6 + 16 * Sizes.Length;
            for (int i = 0; i < Sizes.Length; i++) {
                byte dim = (byte)(Sizes[i] >= 256 ? 0 : Sizes[i]);
                w.Write(dim);
                w.Write(dim);
                w.Write((byte)0);
                w.Write((byte)0);
                w.Write((short)1);
                w.Write((short)32);
                w.Write(images[i].Length);
                w.Write(offset);
                offset += images[i].Length;
            }
            foreach (var image in images) w.Write(image);
        }
    }

    // Every size on a dark and a light strip, plus 8x zooms of the small sizes.
    public static void WritePreview(string path) {
        using (var strip = new Bitmap(900, 420))
        using (var g = Graphics.FromImage(strip)) {
            g.Clear(Color.FromArgb(32, 32, 32));
            g.FillRectangle(new SolidBrush(Color.FromArgb(243, 243, 243)), 0, 100, 900, 100);
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            for (int row = 0; row < 2; row++) {
                int x = 10;
                foreach (int s in new[] { 16, 20, 24, 32, 48, 64 }) {
                    using (var on = Draw(s, false)) g.DrawImage(on, x, 10 + row * 100);
                    using (var off = Draw(s, true)) g.DrawImage(off, x, 10 + row * 100 + 70 - s);
                    x += s + 20;
                }
            }
            int zx = 10;
            foreach (int s in new[] { 16, 20, 24 }) {
                using (var on = Draw(s, false)) g.DrawImage(on, new Rectangle(zx, 210, s * 8, s * 8));
                zx += s * 8 + 20;
            }
            strip.Save(path, ImageFormat.Png);
        }
    }
}
'@
$OutDir = (Resolve-Path $OutDir).Path
[IconMaker]::WriteIco((Join-Path $OutDir 'app.ico'), $false)
[IconMaker]::WriteIco((Join-Path $OutDir 'paused.ico'), $true)
if ($Preview) { [IconMaker]::WritePreview($Preview) }
"Icons written to $OutDir"
