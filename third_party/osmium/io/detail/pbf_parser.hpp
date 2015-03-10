#ifndef OSMIUM_IO_DETAIL_PBF_PRIMITIVE_BLOCK_PARSER_HPP
#define OSMIUM_IO_DETAIL_PBF_PRIMITIVE_BLOCK_PARSER_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013,2014 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <algorithm>

#include <osmpbf/osmpbf.h>

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/io/detail/pbf.hpp> // IWYU pragma: export
#include <osmium/io/detail/zlib.hpp>
#include <osmium/io/header.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/util/cast.hpp>

namespace osmium {

    namespace io {

        namespace detail {

            class PBFPrimitiveBlockParser {

                static constexpr size_t initial_buffer_size = 2 * 1024 * 1024;

                const std::string& m_data;

                const OSMPBF::StringTable* m_stringtable;
                int64_t m_lon_offset;
                int64_t m_lat_offset;
                int64_t m_date_factor;
                int32_t m_granularity;

                osmium::osm_entity_bits::type m_read_types;

                osmium::memory::Buffer m_buffer;

                PBFPrimitiveBlockParser(const PBFPrimitiveBlockParser&) = delete;
                PBFPrimitiveBlockParser(PBFPrimitiveBlockParser&&) = delete;

                PBFPrimitiveBlockParser& operator=(const PBFPrimitiveBlockParser&) = delete;
                PBFPrimitiveBlockParser& operator=(PBFPrimitiveBlockParser&&) = delete;

            public:

                explicit PBFPrimitiveBlockParser(const std::string& data, osmium::osm_entity_bits::type read_types) :
                    m_data(data),
                    m_stringtable(nullptr),
                    m_lon_offset(0),
                    m_lat_offset(0),
                    m_date_factor(1000),
                    m_granularity(100),
                    m_read_types(read_types),
                    m_buffer(initial_buffer_size) {
                }

                ~PBFPrimitiveBlockParser() = default;

                osmium::memory::Buffer operator()() {
                    OSMPBF::PrimitiveBlock pbf_primitive_block;
                    if (!pbf_primitive_block.ParseFromString(m_data)) {
                        throw osmium::pbf_error("failed to parse PrimitiveBlock");
                    }

                    m_stringtable = &pbf_primitive_block.stringtable();
                    m_lon_offset  = pbf_primitive_block.lon_offset();
                    m_lat_offset  = pbf_primitive_block.lat_offset();
                    m_date_factor = pbf_primitive_block.date_granularity() / 1000;
                    m_granularity = pbf_primitive_block.granularity();

                    for (int i=0; i < pbf_primitive_block.primitivegroup_size(); ++i) {
                        const OSMPBF::PrimitiveGroup& group = pbf_primitive_block.primitivegroup(i);

                        if (group.has_dense())  {
                            if (m_read_types & osmium::osm_entity_bits::node) parse_dense_node_group(group);
                        } else if (group.ways_size() != 0) {
                            if (m_read_types & osmium::osm_entity_bits::way) parse_way_group(group);
                        } else if (group.relations_size() != 0) {
                            if (m_read_types & osmium::osm_entity_bits::relation) parse_relation_group(group);
                        } else if (group.nodes_size() != 0) {
                            if (m_read_types & osmium::osm_entity_bits::node) parse_node_group(group);
                        } else {
                            throw osmium::pbf_error("group of unknown type");
                        }
                    }

                    return std::move(m_buffer);
                }

            private:

                template <class TBuilder, class TPBFObject>
                void parse_attributes(TBuilder& builder, const TPBFObject& pbf_object) {
                    auto& object = builder.object();

                    object.set_id(pbf_object.id());

                    if (pbf_object.has_info()) {
                        object.set_version(static_cast_with_assert<object_version_type>(pbf_object.info().version()))
                            .set_changeset(static_cast_with_assert<changeset_id_type>(pbf_object.info().changeset()))
                            .set_timestamp(pbf_object.info().timestamp() * m_date_factor)
                            .set_uid_from_signed(pbf_object.info().uid());
                        if (pbf_object.info().has_visible()) {
                            object.set_visible(pbf_object.info().visible());
                        }
                        builder.add_user(m_stringtable->s(static_cast_with_assert<int>(pbf_object.info().user_sid())));
                    } else {
                        builder.add_user("", 1);
                    }
                }

                void parse_node_group(const OSMPBF::PrimitiveGroup& group) {
                    for (int i=0; i < group.nodes_size(); ++i) {
                        osmium::builder::NodeBuilder builder(m_buffer);
                        const OSMPBF::Node& pbf_node = group.nodes(i);
                        parse_attributes(builder, pbf_node);

                        if (builder.object().visible()) {
                            builder.object().set_location(osmium::Location(
                                              (pbf_node.lon() * m_granularity + m_lon_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision),
                                              (pbf_node.lat() * m_granularity + m_lat_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision)));
                        }

                        if (pbf_node.keys_size() > 0) {
                            osmium::builder::TagListBuilder tl_builder(m_buffer, &builder);
                            for (int tag=0; tag < pbf_node.keys_size(); ++tag) {
                                tl_builder.add_tag(m_stringtable->s(static_cast<int>(pbf_node.keys(tag))),
                                                   m_stringtable->s(static_cast<int>(pbf_node.vals(tag))));
                            }
                        }

                        m_buffer.commit();
                    }
                }

                void parse_way_group(const OSMPBF::PrimitiveGroup& group) {
                    for (int i=0; i < group.ways_size(); ++i) {
                        osmium::builder::WayBuilder builder(m_buffer);
                        const OSMPBF::Way& pbf_way = group.ways(i);
                        parse_attributes(builder, pbf_way);

                        if (pbf_way.refs_size() > 0) {
                            osmium::builder::WayNodeListBuilder wnl_builder(m_buffer, &builder);
                            int64_t ref = 0;
                            for (int n=0; n < pbf_way.refs_size(); ++n) {
                                ref += pbf_way.refs(n);
                                wnl_builder.add_node_ref(ref);
                            }
                        }

                        if (pbf_way.keys_size() > 0) {
                            osmium::builder::TagListBuilder tl_builder(m_buffer, &builder);
                            for (int tag=0; tag < pbf_way.keys_size(); ++tag) {
                                tl_builder.add_tag(m_stringtable->s(static_cast<int>(pbf_way.keys(tag))),
                                                   m_stringtable->s(static_cast<int>(pbf_way.vals(tag))));
                            }
                        }

                        m_buffer.commit();
                    }
                }

                void parse_relation_group(const OSMPBF::PrimitiveGroup& group) {
                    for (int i=0; i < group.relations_size(); ++i) {
                        osmium::builder::RelationBuilder builder(m_buffer);
                        const OSMPBF::Relation& pbf_relation = group.relations(i);
                        parse_attributes(builder, pbf_relation);

                        if (pbf_relation.types_size() > 0) {
                            osmium::builder::RelationMemberListBuilder rml_builder(m_buffer, &builder);
                            int64_t ref = 0;
                            for (int n=0; n < pbf_relation.types_size(); ++n) {
                                ref += pbf_relation.memids(n);
                                rml_builder.add_member(osmpbf_membertype_to_item_type(pbf_relation.types(n)), ref, m_stringtable->s(pbf_relation.roles_sid(n)));
                            }
                        }

                        if (pbf_relation.keys_size() > 0) {
                            osmium::builder::TagListBuilder tl_builder(m_buffer, &builder);
                            for (int tag=0; tag < pbf_relation.keys_size(); ++tag) {
                                tl_builder.add_tag(m_stringtable->s(static_cast<int>(pbf_relation.keys(tag))),
                                                   m_stringtable->s(static_cast<int>(pbf_relation.vals(tag))));
                            }
                        }

                        m_buffer.commit();
                    }
                }

                int add_tags(const OSMPBF::DenseNodes& dense, int n, osmium::builder::NodeBuilder* builder) {
                    if (n >= dense.keys_vals_size()) {
                        return n;
                    }

                    if (dense.keys_vals(n) == 0) {
                        return n+1;
                    }

                    osmium::builder::TagListBuilder tl_builder(m_buffer, builder);

                    while (n < dense.keys_vals_size()) {
                        int tag_key_pos = dense.keys_vals(n++);

                        if (tag_key_pos == 0) {
                            break;
                        }

                        tl_builder.add_tag(m_stringtable->s(tag_key_pos),
                                           m_stringtable->s(dense.keys_vals(n)));

                        ++n;
                    }

                    return n;
                }

                void parse_dense_node_group(const OSMPBF::PrimitiveGroup& group) {
                    int64_t last_dense_id        = 0;
                    int64_t last_dense_latitude  = 0;
                    int64_t last_dense_longitude = 0;
                    int64_t last_dense_uid       = 0;
                    int64_t last_dense_user_sid  = 0;
                    int64_t last_dense_changeset = 0;
                    int64_t last_dense_timestamp = 0;
                    int     last_dense_tag       = 0;

                    const OSMPBF::DenseNodes& dense = group.dense();

                    for (int i=0; i < dense.id_size(); ++i) {
                        bool visible = true;

                        last_dense_id        += dense.id(i);
                        last_dense_latitude  += dense.lat(i);
                        last_dense_longitude += dense.lon(i);

                        if (dense.has_denseinfo()) {
                            last_dense_changeset += dense.denseinfo().changeset(i);
                            last_dense_timestamp += dense.denseinfo().timestamp(i);
                            last_dense_uid       += dense.denseinfo().uid(i);
                            last_dense_user_sid  += dense.denseinfo().user_sid(i);
                            if (dense.denseinfo().visible_size() > 0) {
                                visible = dense.denseinfo().visible(i);
                            }
                            assert(last_dense_changeset >= 0);
                            assert(last_dense_timestamp >= 0);
                            assert(last_dense_uid >= -1);
                            assert(last_dense_user_sid >= 0);
                        }

                        osmium::builder::NodeBuilder builder(m_buffer);
                        osmium::Node& node = builder.object();

                        node.set_id(last_dense_id);

                        if (dense.has_denseinfo()) {
                            auto v = dense.denseinfo().version(i);
                            assert(v > 0);
                            node.set_version(static_cast<osmium::object_version_type>(v));
                            node.set_changeset(static_cast<osmium::changeset_id_type>(last_dense_changeset));
                            node.set_timestamp(last_dense_timestamp * m_date_factor);
                            node.set_uid_from_signed(static_cast<osmium::signed_user_id_type>(last_dense_uid));
                            node.set_visible(visible);
                            builder.add_user(m_stringtable->s(static_cast<int>(last_dense_user_sid)));
                        } else {
                            builder.add_user("", 1);
                        }

                        if (visible) {
                            builder.object().set_location(osmium::Location(
                                              (last_dense_longitude * m_granularity + m_lon_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision),
                                              (last_dense_latitude  * m_granularity + m_lat_offset) / (OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision)));
                        }

                        last_dense_tag = add_tags(dense, last_dense_tag, &builder);
                        m_buffer.commit();
                    }
                }

            }; // class PBFPrimitiveBlockParser

            /**
             * PBF blobs can optionally be packed with the zlib algorithm.
             * This function returns the raw data (if it was unpacked) or
             * the unpacked data (if it was packed).
             *
             * @param input_data Reference to input data.
             * @returns Unpacked data
             * @throws osmium::pbf_error If there was a problem parsing the PBF
             */
            inline std::unique_ptr<const std::string> unpack_blob(const std::string& input_data) {
                OSMPBF::Blob pbf_blob;
                if (!pbf_blob.ParseFromString(input_data)) {
                    throw osmium::pbf_error("failed to parse blob");
                }

                if (pbf_blob.has_raw()) {
                    return std::unique_ptr<std::string>(pbf_blob.release_raw());
                } else if (pbf_blob.has_zlib_data()) {
                    auto raw_size = pbf_blob.raw_size();
                    assert(raw_size >= 0);
                    assert(raw_size <= OSMPBF::max_uncompressed_blob_size);
                    return osmium::io::detail::zlib_uncompress(pbf_blob.zlib_data(), static_cast<unsigned long>(raw_size));
                } else if (pbf_blob.has_lzma_data()) {
                    throw osmium::pbf_error("lzma blobs not implemented");
                } else {
                    throw osmium::pbf_error("blob contains no data");
                }
            }

            /**
             * Parse blob as a HeaderBlock.
             *
             * @param input_buffer Blob data
             * @returns Header object
             * @throws osmium::pbf_error If there was a parsing error
             */
            inline osmium::io::Header parse_header_blob(const std::string& input_buffer) {
                const std::unique_ptr<const std::string> data = unpack_blob(input_buffer);

                OSMPBF::HeaderBlock pbf_header_block;
                if (!pbf_header_block.ParseFromString(*data)) {
                    throw osmium::pbf_error("failed to parse HeaderBlock");
                }

                osmium::io::Header header;
                for (int i=0; i < pbf_header_block.required_features_size(); ++i) {
                    const std::string& feature = pbf_header_block.required_features(i);

                    if (feature == "OsmSchema-V0.6") continue;
                    if (feature == "DenseNodes") {
                        header.set("pbf_dense_nodes", true);
                        continue;
                    }
                    if (feature == "HistoricalInformation") {
                        header.set_has_multiple_object_versions(true);
                        continue;
                    }

                    throw osmium::pbf_error(std::string("required feature not supported: ") + feature);
                }

                for (int i=0; i < pbf_header_block.optional_features_size(); ++i) {
                    const std::string& feature = pbf_header_block.optional_features(i);
                    header.set("pbf_optional_feature_" + std::to_string(i), feature);
                }

                if (pbf_header_block.has_writingprogram()) {
                    header.set("generator", pbf_header_block.writingprogram());
                }

                if (pbf_header_block.has_bbox()) {
                    const OSMPBF::HeaderBBox& pbf_bbox = pbf_header_block.bbox();
                    const int64_t resolution_convert = OSMPBF::lonlat_resolution / osmium::Location::coordinate_precision;
                    osmium::Box box;
                    box.extend(osmium::Location(pbf_bbox.left()  / resolution_convert, pbf_bbox.bottom() / resolution_convert));
                    box.extend(osmium::Location(pbf_bbox.right() / resolution_convert, pbf_bbox.top()    / resolution_convert));
                    header.add_box(box);
                }

                if (pbf_header_block.has_osmosis_replication_timestamp()) {
                    header.set("osmosis_replication_timestamp", osmium::Timestamp(pbf_header_block.osmosis_replication_timestamp()).to_iso());
                }

                if (pbf_header_block.has_osmosis_replication_sequence_number()) {
                    header.set("osmosis_replication_sequence_number", std::to_string(pbf_header_block.osmosis_replication_sequence_number()));
                }

                if (pbf_header_block.has_osmosis_replication_base_url()) {
                    header.set("osmosis_replication_base_url", pbf_header_block.osmosis_replication_base_url());
                }

                return header;
            }

            class DataBlobParser {

                std::string m_input_buffer;
                osmium::osm_entity_bits::type m_read_types;

            public:

                DataBlobParser(std::string&& input_buffer, osmium::osm_entity_bits::type read_types) :
                    m_input_buffer(std::move(input_buffer)),
                    m_read_types(read_types) {
                    if (input_buffer.size() > OSMPBF::max_uncompressed_blob_size) {
                        throw osmium::pbf_error(std::string("invalid blob size: " + std::to_string(input_buffer.size())));
                    }
                }

                DataBlobParser(const DataBlobParser& other) :
                    m_input_buffer(std::move(other.m_input_buffer)),
                    m_read_types(other.m_read_types) {
                }

                DataBlobParser& operator=(const DataBlobParser&) = delete;

                osmium::memory::Buffer operator()() {
                    const std::unique_ptr<const std::string> data = unpack_blob(m_input_buffer);
                    PBFPrimitiveBlockParser parser(*data, m_read_types);
                    return std::move(parser());
                }

            }; // class DataBlobParser

        } // namespace detail

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_DETAIL_PBF_PRIMITIVE_BLOCK_PARSER_HPP
