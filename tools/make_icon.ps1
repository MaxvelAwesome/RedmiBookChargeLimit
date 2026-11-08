# Генерация иконок приложения и трея (многоразмерные .ico с PNG-кадрами).
# Запуск: powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [Math]::Min($r * 2, [Math]::Min($w, $h))
    if ($d -le 0) { $p.AddRectangle((New-Object System.Drawing.RectangleF $x, $y, $w, $h)); return $p }
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function C([string]$hex, [int]$a = 255) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    return [System.Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B)
}

# Молния (нормированные координаты 0..1 внутри прямоугольника).
function Add-Bolt($g, $brush, [single]$x, [single]$y, [single]$w, [single]$h) {
    $pts = @(@(0.58, 0.0), @(0.12, 0.56), @(0.46, 0.56), @(0.36, 1.0), @(0.88, 0.40), @(0.54, 0.40), @(0.66, 0.0)) |
        ForEach-Object { New-Object System.Drawing.PointF ($x + $_[0] * $w), ($y + $_[1] * $h) }
    $g.FillPolygon($brush, [System.Drawing.PointF[]]$pts)
}

# Батарея: корпус-контур, полюс и заливка уровня.
function Draw-Battery($g, [single]$s, [single]$cx, [single]$cy, [single]$bw, [single]$bh,
                      $outline, $fillColor, [single]$level, [bool]$bolt, $boltColor) {
    $stroke = [Math]::Max(1.0, $s / 16.0)
    if ($s -le 20) { $stroke = 1.0 }
    $nubW = [Math]::Max(1.5, $bw * 0.07)
    $x = $cx - ($bw + $nubW) / 2
    $y = $cy - $bh / 2
    $r = $bh * 0.22
    $pen = New-Object System.Drawing.Pen $outline, $stroke
    $half = $stroke / 2
    $body = New-RoundRect ($x + $half) ($y + $half) ($bw - $stroke) ($bh - $stroke) $r
    $g.DrawPath($pen, $body)
    $nubH = $bh * 0.38
    $nb = New-Object System.Drawing.SolidBrush $outline
    $g.FillPath($nb, (New-RoundRect ($x + $bw + $stroke * 0.3) ($cy - $nubH / 2) $nubW $nubH ($nubW * 0.45)))
    $gap = [Math]::Max(1.0, $stroke * 1.2)
    if ($s -le 20) { $gap = 1.0 }
    $ix = $x + $stroke + $gap; $iy = $y + $stroke + $gap
    $iw = $bw - 2 * ($stroke + $gap); $ih = $bh - 2 * ($stroke + $gap)
    $fw = [Math]::Max(1.0, $iw * $level)
    $fb = New-Object System.Drawing.SolidBrush $fillColor
    $g.FillPath($fb, (New-RoundRect $ix $iy $fw $ih ([Math]::Max(0.5, $r - $stroke - $gap))))
    if ($bolt) {
        $bb = New-Object System.Drawing.SolidBrush $boltColor
        Add-Bolt $g $bb ($cx - $bh * 0.30 - $nubW / 2) ($y - $bh * 0.05) ($bh * 0.60) ($bh * 1.10)
    }
}

function Render([int]$s, [string]$kind) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    switch ($kind) {
        'app' {
            # Плитка со скруглением и зелёным градиентом, белая батарея с уровнем 80% и молнией.
            $m = [single]($s * 0.04)
            $tile = New-RoundRect $m $m ($s - 2 * $m) ($s - 2 * $m) ($s * 0.22)
            $lg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $s, $s), (C '#3FE07A'), (C '#0E9F4A')
            $g.FillPath($lg, $tile)
            $hl = New-Object System.Drawing.SolidBrush (C '#FFFFFF' 28)
            $g.FillPath($hl, (New-RoundRect $m $m ($s - 2 * $m) (($s - 2 * $m) * 0.5) ($s * 0.22)))
            Draw-Battery $g $s ($s / 2) ($s / 2) ($s * 0.62) ($s * 0.36) (C '#FFFFFF') (C '#FFFFFF' 235) 0.8 $false $null
            if ($s -ge 24) {
                $bb = New-Object System.Drawing.SolidBrush (C '#12A150')
                $bh = $s * 0.36
                Add-Bolt $g $bb ($s / 2 - $bh * 0.33 - $s * 0.02) ($s / 2 - $bh * 0.42) ($bh * 0.62) ($bh * 0.84)
            }
        }
        'on'   { Draw-Battery $g $s ($s / 2) ($s / 2) ($s * 0.84) ($s * 0.52) (C '#8C8C8C') (C '#2FC562') 0.8 $false $null }
        'off'  { Draw-Battery $g $s ($s / 2) ($s / 2) ($s * 0.84) ($s * 0.52) (C '#8C8C8C') (C '#8C8C8C') 1.0 $false $null }
        'full' { Draw-Battery $g $s ($s / 2) ($s / 2) ($s * 0.84) ($s * 0.52) (C '#8C8C8C') (C '#2FC562') 1.0 $true (C '#FFFFFF') }
    }
    $g.Dispose()
    if ($s -ge 256) {
        # Большой кадр — PNG (стандарт для 256 px).
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        return , $ms.ToArray()
    }
    # Мелкие кадры — классический DIB: BITMAPINFOHEADER + 32bpp BGRA (снизу вверх) + маска AND.
    $rect = New-Object System.Drawing.Rectangle 0, 0, $s, $s
    $bd = $bmp.LockBits($rect, 'ReadOnly', ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    $px = New-Object byte[] ($s * $s * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $px, 0, $px.Length)
    $bmp.UnlockBits($bd)
    $bmp.Dispose()
    $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([UInt32]40); $w.Write([Int32]$s); $w.Write([Int32]($s * 2))
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]0)
    $w.Write([UInt32]($s * $s * 4 + $maskStride * $s)); $w.Write([Int32]0); $w.Write([Int32]0)
    $w.Write([UInt32]0); $w.Write([UInt32]0)
    for ($y = $s - 1; $y -ge 0; $y--) { $w.Write($px, $y * $s * 4, $s * 4) }
    $w.Write((New-Object byte[] ($maskStride * $s)))  # маска не нужна при наличии альфа-канала
    $w.Flush()
    return , $ms.ToArray()
}

function Write-Ico([string]$path, [int[]]$sizes, [string]$kind) {
    $frames = foreach ($s in $sizes) { , (Render $s $kind) }
    $fs = [System.IO.File]::Create($path)
    $w = New-Object System.IO.BinaryWriter $fs
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]; $data = $frames[$i]
        $b = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32)
        $w.Write([UInt32]$data.Length); $w.Write([UInt32]$offset)
        $offset += $data.Length
    }
    foreach ($d in $frames) { $w.Write([byte[]]$d) }
    $w.Close()
    Write-Host "  $path"
}

Write-Host 'Icons:'
Write-Ico (Join-Path $resDir 'app.ico')       @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256) 'app'
Write-Ico (Join-Path $resDir 'tray_on.ico')   @(16, 20, 24, 32, 40, 48) 'on'
Write-Ico (Join-Path $resDir 'tray_off.ico')  @(16, 20, 24, 32, 40, 48) 'off'
Write-Ico (Join-Path $resDir 'tray_full.ico') @(16, 20, 24, 32, 40, 48) 'full'

# Превью для проверки.
$prev = Join-Path $resDir 'icon_preview.png'
[System.IO.File]::WriteAllBytes($prev, (Render 256 'app'))
