param([switch]$Supervisor)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$assetDir=Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Path $assetDir -Force | Out-Null
$frames=[Collections.Generic.List[byte[]]]::new()
foreach($size in @(16,24,32,48,64,128,256)) {
 $bitmap=[Drawing.Bitmap]::new($size,$size)
 $graphics=[Drawing.Graphics]::FromImage($bitmap)
 $graphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
 $graphics.ScaleTransform($size/256.0,$size/256.0)
 $brush=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#1369B5'))
 $graphics.FillEllipse($brush,4,4,248,248)
 $glass=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#45D9D0'))
 $graphics.FillEllipse($glass,47,43,127,127)
 $rim=[Drawing.Pen]::new([Drawing.Color]::White,15)
 $graphics.DrawEllipse($rim,47,43,127,127)
 $handle=[Drawing.Pen]::new([Drawing.Color]::White,25)
 $handle.StartCap=[Drawing.Drawing2D.LineCap]::Round
 $handle.EndCap=[Drawing.Drawing2D.LineCap]::Round
 $graphics.DrawLine($handle,158,154,206,202)
 $pen=[Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#FFCE47'),15)
 $graphics.DrawLine($pen,73,130,135,69)
 $graphics.FillPolygon([Drawing.Brushes]::White,[Drawing.PointF[]]@([Drawing.PointF]::new(62,143),[Drawing.PointF]::new(68,118),[Drawing.PointF]::new(88,137)))
 if($Supervisor) {
  $shield=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#19A86B'))
  $outline=[Drawing.Pen]::new([Drawing.Color]::White,8)
  $points=[Drawing.PointF[]]@([Drawing.PointF]::new(155,164),[Drawing.PointF]::new(199,147),[Drawing.PointF]::new(243,164),[Drawing.PointF]::new(236,213),[Drawing.PointF]::new(199,245),[Drawing.PointF]::new(162,213))
  $graphics.FillPolygon($shield,$points)
  $graphics.DrawPolygon($outline,$points)
  $check=[Drawing.Pen]::new([Drawing.Color]::White,11)
  $check.StartCap=[Drawing.Drawing2D.LineCap]::Round
  $check.EndCap=[Drawing.Drawing2D.LineCap]::Round
  $graphics.DrawLines($check,[Drawing.PointF[]]@([Drawing.PointF]::new(177,193),[Drawing.PointF]::new(194,211),[Drawing.PointF]::new(224,179)))
  $check.Dispose()
  $outline.Dispose()
  $shield.Dispose()
 }
 $stream=[IO.MemoryStream]::new()
 $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
 $frames.Add($stream.ToArray())
 $imageName=if($Supervisor){"supervisor-$size.png"}else{"icon-$size.png"}
 $bitmap.Save((Join-Path $assetDir $imageName),[Drawing.Imaging.ImageFormat]::Png)
 $stream.Dispose();$pen.Dispose();$handle.Dispose();$rim.Dispose();$glass.Dispose();$brush.Dispose();$graphics.Dispose();$bitmap.Dispose()
}
$iconName=if($Supervisor){'supervisor.ico'}else{'appicon.ico'}
$file=[IO.File]::Create((Join-Path $assetDir $iconName))
$writer=[IO.BinaryWriter]::new($file)
$writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$frames.Count)
$offset=6+16*$frames.Count
$sizes=@(16,24,32,48,64,128,256)
for($i=0;$i -lt $frames.Count;$i++) {
 $dimension=if($sizes[$i] -eq 256){0}else{$sizes[$i]}
 $writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0)
 $writer.Write([uint16]1);$writer.Write([uint16]32);$writer.Write([uint32]$frames[$i].Length);$writer.Write([uint32]$offset)
 $offset+=$frames[$i].Length
}
foreach($frame in $frames){$writer.Write($frame)}
$writer.Dispose()

