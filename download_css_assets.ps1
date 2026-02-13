$path=".\index.html"
$html = Get-Content $path -Raw -Encoding UTF8

# /#work -> #work, /#pricing -> #pricing, /#faq -> #faq и любые такие
$html = $html -replace 'href="/#', 'href="#'

# логотип/brand: / -> ./ (чтобы не уходило в корень домена)
$html = $html -replace 'href="/"', 'href="./"'

Set-Content $path $html -Encoding UTF8
