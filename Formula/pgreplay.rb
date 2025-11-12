class Pgreplay < Formula
  desc "Record and replay real-life PostgreSQL workloads"
  homepage "https://github.com/laurenz/pgreplay"
  url "https://github.com/laurenz/pgreplay/archive/refs/tags/PGREPLAY_1_3_0.tar.gz"
  sha256 "ff1d67d568df5a23c0ba79b74d7870f0f8711683cda8b9e0a1fafd18c2f33409"
  license "MIT"
  head "https://github.com/laurenz/pgreplay.git", branch: "master"

  livecheck do
    url :stable
    strategy :github_latest
  end

  depends_on "postgresql"

  def install
    pg_config = Formula["postgresql"].opt_bin/"pg_config"
    libdir = Utils.safe_popen_read(pg_config, "--libdir").strip
    includedir = Utils.safe_popen_read(pg_config, "--includedir").strip

    ENV.append "LDFLAGS", "-L#{libdir}"
    ENV.append "CPPFLAGS", "-I#{includedir}"

    system "./configure", "--prefix=#{prefix}", "--with-postgres=#{pg_config.dirname}"

    inreplace "Makefile" do |s|
      s.gsub! "-D -m 0755", "-m 0755"
      s.gsub! "-D -m 0644", "-m 0644"
    end

    bin.mkpath
    man1.mkpath

    system "make"
    system "make", "install"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/pgreplay -v")
  end
end
