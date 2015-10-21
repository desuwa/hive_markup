#!/usr/bin/env ruby
# encoding: utf-8

require 'minitest/autorun'
require 'hive_markup'

include Hive

describe Markup do
  it 'parses quotelinks' do
    Markup.render('>>1').must_equal '<a class="ql" href="#1">&gt;&gt;1</a>'
  end
  
  it 'parses quotes' do
    Markup.render(">quote").must_equal '<span class="q">&gt;quote</span>'
  end
  
  it 'ignores midline quotes' do
    Markup.render("text >notquote").must_equal 'text &gt;notquote'
  end
  
  it 'parses emphases' do
    Markup.render('*em*').must_equal '<em>em</em>'
  end
  
  it 'ignores midword emphases' do
    Markup.render('text*notem*').must_equal 'text*notem*', 'midword start'
    Markup.render('*notem*text').must_equal '*notem*text', 'midword end'
  end
  
  it 'parses spoilers' do
    Markup.render('$$text$$').must_equal '<span class="s">text</span>'
  end
  
  it 'parses multiline spoilers' do
    input = "$$\nspoiler\nspoiler\n$$"
    Markup.render(input).must_match(/^<span class="s">.+<\/span>$/)
  end
  
  it 'parses fenced AA blocks' do
    Markup.render("~~~\nasciiart\n~~~").must_match(/^<pre class="aa".+<\/pre>$/)
  end
  
  it 'parses fenced code blocks' do
    Markup.render("```\ncode\n```").must_match(/^<pre class="code".+<\/pre>$/)
  end
  
  it 'ignores AA blocks inside spoilers' do
    input = "$$\n~~~\nasciiart\n~~~\n$$"
    expected = /^<span class="s">~~~.+~~~<\/span>$/
    Markup.render(input).must_match expected
  end
  
  it 'ignores code blocks inside spoilers' do
    input = "$$\n```\ncode\n```\n$$"
    expected = /^<span class="s">```.+```<\/span>$/
    Markup.render(input).must_match expected
  end
  
  it 'parses linebreaks' do
    Markup.render("\n").must_equal '<br>'
  end
  
  it 'collapses 3+ linebreaks into 2' do
    Markup.render("\n\n\n").must_equal '<br><br>'
  end
  
  it 'expands tabs into 2 spaces' do
    Markup.render("\tline").must_equal '  line'
  end
  
  it 'skips ASCII control chars' do
    input = (((0..8).to_a + (11..31).to_a) << 127).pack('c*')
    Markup.render(input).must_equal ''
  end
  
  describe 'Autolinking' do
    it 'autolinks http' do
      input = 'http://abc'
      expected = '<a href="http:&#47;&#47;abc">http:&#47;&#47;abc</a>'
      Markup.render(input).must_equal expected
    end
    
    it 'autolinks https' do
      input = 'https://abc'
      expected = '<a href="https:&#47;&#47;abc">https:&#47;&#47;abc</a>'
      Markup.render(input).must_equal expected
    end
    
    it 'strips trailing punctuation from autolinks' do
      in_p = ":;!?,.'\"&"
      out_p = ':;!?,.&#39;&quot;&amp;'
      
      input = 'http://abc' << in_p
      expected = '<a href="http:&#47;&#47;abc">http:&#47;&#47;abc</a>' << out_p
      Markup.render(input).must_equal expected, 'Punctuation'
    end
    
    it 'keeps inner parens in urls' do
      input = 'http://ab(c))))'
      expected = '<a href="http:&#47;&#47;ab(c)">http:&#47;&#47;ab(c)</a>)))'
      Markup.render(input).must_equal expected
    end
    
    it 'ignores outer parens in urls' do
      input = '(http://abc)))'
      expected = '(<a href="http:&#47;&#47;abc">http:&#47;&#47;abc</a>)))'
      Markup.render(input).must_equal expected
    end
  end
  
  it 'escapes html' do
    input = <<-TXT.gsub(/^\s+/, '')
      <*em*> <>>1< &$$<spoiler<$$&
      ~~~
      'aa<
      ~~~
      "
      ```
      'code<
      ```
      <https://abc<
    TXT
    
    expected = '&lt;<em>em</em>&gt; &lt;<a class="ql" href="#1">&gt;&gt;1' <<
      '</a>&lt; &amp;<span class="s">&lt;spoiler&lt;</span>&amp;<br>' <<
      '<pre class="aa">&#39;aa&lt;</pre>&quot;<br><pre class="code"><code class=' << 
      '"prettyprint">&#39;code&lt;</code></pre>&lt;<a href="' <<
      'https:&#47;&#47;abc&lt;">https:&#47;&#47;abc&lt;</a><br>'
      
    Markup.render(input).must_equal expected
  end
  
  it "doesn't choke on ambiguous quoting markup" do
    assert Markup.render('>>1a')
    assert Markup.render('>1')
  end
end
